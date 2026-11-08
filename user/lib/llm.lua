-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- **The LLM server, asked** (`docs/xavier.md` X3): `/Running/llm` in
-- `llmproto.h`'s shapes, and the answer read out of the region it hands
-- back (`llmring.h`). Xavier is the first to use it; anything that wants an
-- answer from a model uses the same door rather than a copy of it.
--
--   local llm = use("/Kosmos/Libraries/llm.lua")
--   local info, why = llm.load("phi-2.Q4_0.gguf")
--   local a = llm.ask("Instruct: What is a kernel?\nOutput:", { tokens = 64 })
--   while true do
--     local text = a:wait()        -- what has come since the last call
--     if not text then break end   -- the answer has ended
--     write(text)
--   end
--
-- **Nothing here polls.** `wait` is held by the server until the answer has
-- moved, and then the new bytes are read out of the region, where they
-- were written - the text never travels in a message.

local llm = {}

local PATH = "/Running/llm"

local OP_LOAD, OP_ASK, OP_WAIT, OP_STOP, OP_STATS = 1, 2, 3, 4, 5

local STOP_AT_BLANK_LINE = 1

local TEXT_MAX = 1536

-- `struct llm_request`, 1624 bytes, and `struct llm_reply`, 336.
local REQUEST = "<I4I4I4I4I4I4c64c1536"
local REPLY = "<I4I4I4I4c64c64c16c96I8I8I4I4I4I4I4I4I8I8I8I8I8"
local REPLY_SIZE = 336

-- `struct llm_answer`'s fields, where they are in the region.
local ANSWER_DATA = 4096
local AT_WRITTEN, AT_ENDED = 8, 12

local ERRORS = {
  [1] = "the LLM server does not know that request",
  [2] = "there is no such model in /Home/Models",
  [3] = "that file is not a model the LLM Kit runs",
  [4] = "there is not memory enough for that model",
  [5] = "no model is loaded",
  [6] = "the LLM server is busy with another answer",
  [7] = "the question is longer than the model can hold",
  [8] = "there is no answer to wait on",
}

llm.STATES = { [0] = "empty", "loading", "ready", "answering" }

local function cstring(s)
  return (s:gsub("%z.*", ""))
end

local function pad(s, n)
  s = s or ""
  if #s > n then s = s:sub(1, n) end
  return s .. string.rep("\0", n - #s)
end

-- One request; the reply as a table, and the capability that came with it.
local function call(op, fields)
  fields = fields or {}
  local text = fields.text or ""
  local bytes = string.pack(REQUEST, op, fields.tokens or 0, fields.flags or 0,
                            fields.since or 0, #text, 0, pad(fields.name, 64),
                            pad(text, TEXT_MAX))
  local reply, cap = fs.raw(PATH, bytes)

  if not reply then
    return nil, "the LLM server is not running (" .. tostring(cap) .. ")"
  end

  if #reply < REPLY_SIZE then
    return nil, "the LLM server sent a reply of the wrong size"
  end

  local err, state, written, ended, model, title, arch, why, model_bytes, loaded_bytes,
        n_layer, n_embd, n_vocab, n_ctx, prompt_tokens, answer_tokens,
        prompt_us, answer_us, first_us, load_us, answers = string.unpack(REPLY, reply)

  local r = {
    error = err, state = llm.STATES[state] or tostring(state), written = written,
    ended = ended ~= 0, model = cstring(model), title = cstring(title),
    arch = cstring(arch), why = cstring(why), model_bytes = model_bytes,
    loaded_bytes = loaded_bytes, n_layer = n_layer, n_embd = n_embd,
    n_vocab = n_vocab, n_ctx = n_ctx, prompt_tokens = prompt_tokens,
    answer_tokens = answer_tokens, prompt_us = prompt_us, answer_us = answer_us,
    first_us = first_us, load_us = load_us, answers = answers,
  }

  -- A reply with no capability says -1.
  if type(cap) ~= "number" or cap < 0 then cap = nil end

  if err ~= 0 then
    if cap then sys.release(cap) end
    local said = ERRORS[err] or ("the LLM server refused, error " .. err)
    -- `why` is the last load's: the kit's or the disk's words for a refusal.
    if op == OP_LOAD and r.why ~= "" then said = said .. ": " .. r.why end
    return nil, said, r
  end

  return r, cap
end

-- What is loaded and how the last answer went.
function llm.stats()
  return call(OP_STATS)
end

-- A model, by its file's name in /Home/Models. Returns when it is in.
function llm.load(name)
  return call(OP_LOAD, { name = name })
end

local answer = {}
answer.__index = answer

-- A question, as the text the model is to continue. `options.tokens` is
-- the most to answer with (128 when not said), and `options.paragraph`
-- ends the answer at its first blank line.
function llm.ask(text, options)
  options = options or {}
  if #text > TEXT_MAX then
    return nil, "the question is longer than one request holds (" .. TEXT_MAX .. " bytes)"
  end

  local r, cap = call(OP_ASK, { text = text, tokens = options.tokens,
                                flags = options.paragraph and STOP_AT_BLANK_LINE or 0 })
  if not r then return nil, cap end
  if not cap then return nil, "the LLM server sent no answer region" end

  return setmetatable({ cap = cap, seen = 0, done = false }, answer)
end

-- What has come since the last call - waiting until there is some - or
-- nil once the answer has ended and everything in it has been read.
function answer:wait()
  while not self.done do
    local written = sys.region_load32(self.cap, AT_WRITTEN)
    local ended = sys.region_load32(self.cap, AT_ENDED) ~= 0

    if written > self.seen then
      local text = sys.region_read(self.cap, ANSWER_DATA + self.seen, written - self.seen)
      self.seen = written
      return text
    end

    if ended then
      self:close()
      return nil
    end

    local r, why = call(OP_WAIT, { since = self.seen })
    if not r then
      self:close()
      return nil, why
    end
  end

  return nil
end

-- The whole answer, waited for.
function answer:all()
  local parts = {}
  while true do
    local text, why = self:wait()
    if not text then
      if why then return nil, why end
      break
    end
    parts[#parts + 1] = text
  end
  return table.concat(parts)
end

-- Stop the answer where it is.
function answer:stop()
  return call(OP_STOP)
end

function answer:close()
  if not self.done then
    self.done = true
    sys.release(self.cap)
  end
end

return llm
