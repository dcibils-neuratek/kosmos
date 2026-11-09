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

local OP_LOAD, OP_ASK, OP_WAIT, OP_STOP, OP_STATS, OP_SETTINGS = 1, 2, 3, 4, 5, 6

local STOP_AT_BLANK_LINE, TEXT_IN_REGION, LOAD_NOWAIT, SETTINGS_CHANGE = 1, 2, 4, 8

local TEXT_MAX = 1536
local STOP_MAX = 24
local CONVERSATION_MAX = 256 * 1024

-- `struct llm_request`, 1688 bytes, and `struct llm_reply`, 424.
local REQUEST = "<I4I4I4I4I4I4c64c1536c24c24I4I4I4I4"
local REPLY = "<I4I4I4I4c64c64c16c96I8I8I4I4I4I4I4I4I8I8I8I8I8I4I4I4I4I4I4I4I4I4I4c32I4I4I4I4"
local REPLY_SIZE = 424

-- Which loops, by the protocol's numbers (`llmproto.h`, LLM_LOOPS_*).
llm.LOOPS = { [0] = "auto", "plain", "base", "avx2" }
local LOOP_NUMBER = { auto = 0, plain = 1, base = 2, avx2 = 3 }
llm.GPU_READ, llm.GPU_WRITE = 1, 2

-- `struct llm_answer`'s fields, where they are in the region.
local ANSWER_DATA = 4096
local AT_WRITTEN, AT_ENDED, AT_TOKENS = 8, 12, 16

local ERRORS = {
  [1] = "the LLM server does not know that request",
  [2] = "there is no such model in the models folder",
  [3] = "that file is not a model the LLM Kit runs",
  [4] = "there is not memory enough for that model",
  [5] = "no model is loaded",
  [6] = "the LLM server is busy with another answer",
  [7] = "the question is longer than the model can hold",
  [8] = "there is no answer to wait on",
  [9] = "this machine cannot run those settings",
  [10] = "the settings are in force, but could not be kept beside the models",
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
-- `pass` is a region handed with it: the server copies what it needs and
-- the caller still holds its own.
local function call(op, fields, pass)
  fields = fields or {}
  local text = fields.text or ""
  local stops = fields.stops or {}
  local set = fields.settings or {}
  local bytes = string.pack(REQUEST, op, fields.tokens or 0, fields.flags or 0,
                            fields.since or 0, fields.length or #text, 0, pad(fields.name, 64),
                            pad(text, TEXT_MAX), pad(stops[1], STOP_MAX), pad(stops[2], STOP_MAX),
                            set.cores or 0, set.loops or 0, set.gpu or 0, 0)
  local reply, cap = fs.raw(PATH, bytes, pass)

  if not reply then
    return nil, "the LLM server is not running (" .. tostring(cap) .. ")"
  end

  if #reply < REPLY_SIZE then
    return nil, "the LLM server sent a reply of the wrong size"
  end

  local err, state, written, ended, model, title, arch, why, model_bytes, loaded_bytes,
        n_layer, n_embd, n_vocab, n_ctx, prompt_tokens, answer_tokens,
        prompt_us, answer_us, first_us, load_us, answers, kept_tokens, held_tokens,
        s_cores, s_loops, s_gpu, _, processors, cores, loops_have, gpu_have, gpu,
        ran_cores, ran_loops, ran_gpu = string.unpack(REPLY, reply)

  local r = {
    error = err, state = llm.STATES[state] or tostring(state), written = written,
    ended = ended ~= 0, model = cstring(model), title = cstring(title),
    arch = cstring(arch), why = cstring(why), model_bytes = model_bytes,
    loaded_bytes = loaded_bytes, n_layer = n_layer, n_embd = n_embd,
    n_vocab = n_vocab, n_ctx = n_ctx, prompt_tokens = prompt_tokens,
    answer_tokens = answer_tokens, prompt_us = prompt_us, answer_us = answer_us,
    first_us = first_us, load_us = load_us, answers = answers,
    kept_tokens = kept_tokens, held_tokens = held_tokens,
    -- Where the model runs (`xavier.md`, *Settings*): the server's settings,
    -- what this machine has, and what the last answer ran on.
    settings = { cores = s_cores, loops = llm.LOOPS[s_loops] or "auto", gpu = s_gpu },
    processors = processors, cores = cores, gpu = cstring(gpu), gpu_have = gpu_have,
    loops_have = {}, ran = { cores = ran_cores, loops = llm.LOOPS[ran_loops] or "?", gpu = ran_gpu },
  }

  for n, name in pairs(llm.LOOPS) do
    if n > 0 and loops_have & (1 << n) ~= 0 then r.loops_have[name] = true end
  end

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

--------------------------------------------------------------------------
-- Which model, and how it is asked: the same for every application, so
-- here rather than in each (Xavier's window and `xavier` at the prompt
-- both had a copy of the choice).
--------------------------------------------------------------------------

-- **Where the models are** (`docs/xavier.md`, *Where the models live*):
-- `/Models`, a drive of their own, on a machine started with one; else
-- `/Home/Models`. The boot word decides here as it does in init, which
-- hands the LLM server the same one, so the two cannot disagree.
llm.MODELS = (sys.boot and sys.boot("opt/kosmos/models")) and "/Models" or "/Home/Models"
llm.PREFERRED = { "phi-2.Q4_0.gguf", "phi-2.Q8_0.gguf" }

-- The models there are, by name, in name order.
function llm.models()
  local out = {}

  for _, n in ipairs(fs.list(llm.MODELS) or {}) do
    if n:lower():match("%.gguf$") then out[#out + 1] = n end
  end

  table.sort(out)
  return out
end

-- The one named; else phi-2's, when it is there; else the first there is.
function llm.choose(named)
  if named and named ~= "" then return named end

  local names = llm.models()
  local have = {}

  for _, n in ipairs(names) do have[n] = true end
  for _, n in ipairs(llm.PREFERRED) do if have[n] then return n end end

  return names[1]
end

--
-- **How a model is asked, by its architecture.** phi-2 was trained on
-- `Instruct: ... Output:`, and goes on to write the next `Instruct:` itself
-- unless stopped there. A conversation is those turns one after another,
-- each answer as the model wrote it - which is what lets the server find
-- what its cache already holds at the start of the next.
--
local FORMS = {
  phi2 = {
    question = function(q) return "Instruct: " .. q .. "\nOutput:" end,
    between = "\n",
    stops = { "\nInstruct:", "\nOutput:" },
  },
}

function llm.form(arch)
  return FORMS[arch] or FORMS.phi2
end

-- The text of a conversation - `before`, what the model is told first,
-- then `turns`, each `{ you = , xavier = }`, then `question` - and the
-- stops its answer ends at.
function llm.chat(arch, turns, question, before)
  local form = llm.form(arch)
  local parts = { before or "" }

  for _, t in ipairs(turns) do
    parts[#parts + 1] = form.question(t.you) .. t.xavier .. form.between
  end

  parts[#parts + 1] = form.question(question)
  return table.concat(parts), form.stops
end

-- What is loaded and how the last answer went.
function llm.stats()
  return call(OP_STATS)
end

-- **Where the model runs**: the server's settings, for every application
-- that asks, and what this machine has to choose from - the same reply as
-- `llm.stats`, whose `settings`, `processors`, `cores`, `loops_have` and
-- `ran` say it.
function llm.settings()
  return call(OP_SETTINGS)
end

-- Changed: `{ cores = 0 (Auto) or a number, loops = "auto" | "plain" |
-- "base" | "avx2", gpu = bits }`, any of them; the rest as they stand. In
-- force from the next question, and kept by the server across restarts.
function llm.set(change)
  local now, why = call(OP_SETTINGS)

  if not now then return nil, why end

  local s = now.settings
  local loops = change.loops or s.loops

  if LOOP_NUMBER[loops] == nil then return nil, "no such loops: " .. tostring(loops) end

  return call(OP_SETTINGS, { flags = SETTINGS_CHANGE, settings = {
    cores = change.cores or s.cores, loops = LOOP_NUMBER[loops], gpu = change.gpu or s.gpu } })
end

-- A model, by its file's name in /Home/Models. Returns when it is in - or,
-- with `{ wait = false }`, at once, for a window that watches `llm.stats`
-- on its own clock rather than stop while a gigabyte is read.
function llm.load(name, options)
  local nowait = options and options.wait == false
  return call(OP_LOAD, { name = name, flags = nowait and LOAD_NOWAIT or 0 })
end

local answer = {}
answer.__index = answer

-- A question, as the text the model is to continue - for a conversation,
-- all of it so far: the server keeps what its cache already holds of it and
-- reads only what is new (`llmproto.h`). `options.tokens` is the most to
-- answer with (128 when not said); `options.paragraph` ends the answer at
-- its first blank line; `options.stops`, up to two texts, end it where
-- they begin, and are not in it. Text longer than one message goes in a
-- region handed with the request.
function llm.ask(text, options)
  options = options or {}
  local flags = options.paragraph and STOP_AT_BLANK_LINE or 0
  local fields = { tokens = options.tokens, stops = options.stops }
  local pass

  if #text > CONVERSATION_MAX then
    return nil, "the conversation is longer than the LLM server takes (" .. CONVERSATION_MAX
                .. " bytes)"
  end

  for _, stop in ipairs(options.stops or {}) do
    if #stop > STOP_MAX then return nil, "a stop text is at most " .. STOP_MAX .. " bytes" end
  end

  if #text > TEXT_MAX or options.region then
    local why
    pass, why = sys.memory((#text + 4095) // 4096)

    if not pass then return nil, "no region for the conversation: " .. tostring(why) end

    sys.region_write(pass, 0, text)
    fields.flags, fields.length = flags | TEXT_IN_REGION, #text
  else
    fields.flags, fields.text = flags, text
  end

  local r, cap = call(OP_ASK, fields, pass)

  if pass then sys.release(pass) end

  if not r then return nil, cap end
  if not cap then return nil, "the LLM server sent no answer region" end

  return setmetatable({ cap = cap, seen = 0, done = false, ended = false }, answer)
end

-- Whether a refusal was for length: a conversation that has outgrown the
-- model's memory, which its owner shortens and asks again.
function llm.too_long(why)
  return type(why) == "string" and why == ERRORS[7]
end

-- **The answer so far, never waiting**: what has come since the last
-- call, "" when nothing has, and nil once it has ended and all of it has
-- been read. Two loads from the region and no message, so a window calls
-- it on its own clock (`CLAUDE.md`: nothing on the desktop waits on a
-- server). `ended` is read before `written`: the server publishes the last
-- bytes first and the end after, so the other order could see the end and
-- miss the bytes.
function answer:poll()
  if self.done then return nil end

  local ended = sys.region_load32(self.cap, AT_ENDED) ~= 0
  local written = sys.region_load32(self.cap, AT_WRITTEN)

  if written > self.seen then
    local text = sys.region_read(self.cap, ANSWER_DATA + self.seen, written - self.seen)
    self.seen = written
    return text
  end

  if ended then
    self.ended = true
    self:close()
    return nil
  end

  return ""
end

-- What has come since the last call - waiting until there is some - or
-- nil once the answer has ended and everything in it has been read.
function answer:wait()
  while not self.done do
    -- The end first, then the bytes: see `poll`.
    local ended = sys.region_load32(self.cap, AT_ENDED) ~= 0
    local written = sys.region_load32(self.cap, AT_WRITTEN)

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

-- How many tokens the answer has so far: a load, no message.
function answer:tokens()
  if self.done then return nil end
  return sys.region_load32(self.cap, AT_TOKENS)
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
