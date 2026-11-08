-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- **A conversation with a model, as a file** (`docs/xavier.md` X4): what
-- Xavier keeps in `/Home/Xavier`, one file a conversation, opened again
-- where it was left and thrown away to the Trash like anything else.
--
-- The format is text a person can read and a program cannot misread:
--
--   kosmos conversation 1
--   title What is the capital of France?
--   created 1794150000
--   updated 1794150300
--   model phi-2.Q4_0.gguf
--   held 412 2048
--   forgotten 0
--   turn 31 32 7 400000 700000 500000
--   <31 bytes: the question>
--   <32 bytes: the answer>
--
-- A turn says its question's and answer's lengths in bytes before them -
-- then the answer's tokens and its three times in microseconds: reading the
-- question, writing the answer, and the wait for its first token - so an
-- answer with a line in it that looks like a header is still the answer.
-- `held` is the model's memory the conversation filled, of how much there
-- was; `forgotten` is how many of the first turns no longer fit and are no
-- longer shown to the model, though they are still in the file.

local files = use("/Kosmos/Libraries/files.lua")

local conversations = {}

conversations.DIR = "/Home/Xavier"
conversations.SUFFIX = ".conversation"

local MAGIC = "kosmos conversation 1"

-- A new conversation, not yet on the disk: `save` gives it a file.
function conversations.new(now)
  return { title = "", created = now or 0, updated = now or 0, model = "",
           held = 0, room = 0, forgotten = 0, turns = {} }
end

-- What a conversation is called in a list: its first question, cut.
function conversations.title_of(c)
  if c.title ~= "" then return c.title end

  local first = c.turns[1] and c.turns[1].you or ""

  first = first:gsub("%s+", " "):gsub("^ ", "")
  if first == "" then return "New conversation" end
  if #first > 60 then first = first:sub(1, 57):gsub("[\128-\191]*$", "") .. "..." end

  return first
end

local function number(s) return tonumber(s) or 0 end

-- The file's bytes as a conversation, or nil and why.
function conversations.parse(bytes)
  if type(bytes) ~= "string" then return nil, "not a file" end

  local at = 1
  local c = conversations.new()

  local function line()
    local stop = bytes:find("\n", at, true)

    if not stop then return nil end

    local l = bytes:sub(at, stop - 1)
    at = stop + 1
    return l
  end

  if line() ~= MAGIC then return nil, "not a conversation" end

  while at <= #bytes do
    local l = line()

    if not l then break end

    local key, rest = l:match("^(%S+) ?(.*)$")

    if key == "title" then c.title = rest
    elseif key == "created" then c.created = number(rest)
    elseif key == "updated" then c.updated = number(rest)
    elseif key == "model" then c.model = rest
    elseif key == "held" then
      local h, r = rest:match("^(%d+) (%d+)$")
      c.held, c.room = number(h), number(r)
    elseif key == "forgotten" then c.forgotten = number(rest)
    elseif key == "turn" then
      local qn, an, tokens, read_us, write_us, first_us =
        rest:match("^(%d+) (%d+) (%d+) (%d+) (%d+) (%d+)$")

      qn, an = tonumber(qn), tonumber(an)

      if not qn or at + qn + an + 1 > #bytes + 1 then return nil, "a turn is cut short" end

      local t = { you = bytes:sub(at, at + qn - 1), tokens = number(tokens),
                  read_us = number(read_us), write_us = number(write_us),
                  first_us = number(first_us) }

      at = at + qn + 1                -- and the newline after it
      t.xavier = bytes:sub(at, at + an - 1)
      at = at + an + 1
      c.turns[#c.turns + 1] = t
    end
  end

  return c
end

function conversations.format(c)
  local out = { MAGIC,
                "title " .. (c.title or ""):gsub("\n", " "),
                "created " .. math.floor(c.created or 0),
                "updated " .. math.floor(c.updated or 0),
                "model " .. (c.model or ""),
                ("held %d %d"):format(c.held or 0, c.room or 0),
                "forgotten " .. (c.forgotten or 0) }

  for _, t in ipairs(c.turns) do
    out[#out + 1] = ("turn %d %d %d %d %d %d"):format(#t.you, #t.xavier, t.tokens or 0,
      t.read_us or 0, t.write_us or 0, t.first_us or 0)
    out[#out + 1] = t.you
    out[#out + 1] = t.xavier
  end

  return table.concat(out, "\n") .. "\n"
end

function conversations.read(path)
  local bytes, why = fs.read(path)

  if not bytes then return nil, why end

  local c, said = conversations.parse(bytes)

  if c then c.path = path end

  return c, said
end

-- On the disk: in its own file, or a new one named for when it began.
function conversations.save(c, stamp)
  if not c.path then
    local ok, why = files.make_folder(conversations.DIR)

    if not ok then return nil, why end

    local name, said = files.free_name(conversations.DIR, stamp .. conversations.SUFFIX)

    if not name then return nil, said end

    c.path = files.join(conversations.DIR, name)
  end

  return fs.write(c.path, conversations.format(c))
end

-- Every conversation in the folder, the most recent first. A file that is
-- not one is passed over rather than shown broken.
function conversations.list()
  local out = {}

  for _, name in ipairs(fs.list(conversations.DIR) or {}) do
    if name:sub(-#conversations.SUFFIX) == conversations.SUFFIX then
      local c = conversations.read(files.join(conversations.DIR, name))

      if c then out[#out + 1] = c end
    end
  end

  table.sort(out, function(a, b)
    if a.updated ~= b.updated then return a.updated > b.updated end
    return a.path > b.path
  end)

  return out
end

-- Into the Trash, where it can be taken back from (`files.to_trash`).
function conversations.trash(c)
  if not c.path then return true end

  local ok, why = files.to_trash(c.path)

  if ok then c.path = nil end

  return ok, why
end

return conversations
