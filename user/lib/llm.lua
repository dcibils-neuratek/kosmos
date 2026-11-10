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

local OP_LOAD, OP_ASK, OP_WAIT, OP_STOP, OP_STATS, OP_SETTINGS, OP_INSIDE, OP_LENS = 1, 2, 3, 4, 5, 6, 7, 8

local STOP_AT_BLANK_LINE, TEXT_IN_REGION, LOAD_NOWAIT, SETTINGS_CHANGE = 1, 2, 4, 8

local TEXT_MAX = 1536
local STOP_MAX = 24
local CONVERSATION_MAX = 256 * 1024

-- `struct llm_request`, 1696 bytes, and `struct llm_reply`, 1040: 424 of
-- the model and the last answer, then since the server started and who
-- has asked (`ASKER`, eight of them), then where the last answer's time
-- went (`STAGES`, at 913).
local REQUEST = "<I4I4I4I4I4I4c64c1536c24c24I4I4I4I4I4I4"
local REPLY = "<I4I4I4I4c64c64c16c96I8I8I4I4I4I4I4I4I8I8I8I8I8I4I4I4I4I4I4I4I4I4I4c32I4I4I4I4"
local REPLY_SIZE = 1040
local STAGES, AT_STAGES = "<I8I8I8I8I8I8I8I8", 913

-- The forward pass's stages, as `llm_core.h` numbers them (LLM_STAGE_*).
llm.STAGES = { "embedding", "norms", "products", "rope", "attention", "gelu", "sums" }
local SINCE = "<I8I8I8I4I4I4I4"
local ASKER = "<c16I4I4I4I4I8I8I8"
local ASKERS, ASKER_SIZE = 8, 56

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
  [11] = "that word's layers are not kept - written while nobody watched, or written over since",
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
                            fields.since or 0, fields.length or #text, fields.seed or 0, pad(fields.name, 64),
                            pad(text, TEXT_MAX), pad(stops[1], STOP_MAX), pad(stops[2], STOP_MAX),
                            set.cores or 0, set.loops or 0, set.gpu or 0, 0, fields.record or 0, 0)
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

  -- Since the server started, and who has asked (the LLM Monitor): names
  -- are the kernel's for the process that sent, never the message's own.
  local at
  r.up_us, r.read_tokens, r.written_tokens, r.answering, r.loading_for, r.refused, _, at =
    string.unpack(SINCE, reply, 425)
  r.askers = {}

  for i = 1, ASKERS do
    local name, id, answers, refused, _, read, written, last_us =
      string.unpack(ASKER, reply, at + (i - 1) * ASKER_SIZE)
    name = cstring(name)

    if name ~= "" then
      r.askers[#r.askers + 1] = { name = name, id = id, answers = answers, refused = refused,
                                  read_tokens = read, written_tokens = written, last_us = last_us,
                                  answering = r.answering == i, loading = r.loading_for == i }
    end
  end

  -- Where the last answer's time went, reading and writing apart.
  local read = table.pack(string.unpack(STAGES, reply, AT_STAGES))
  local wrote = table.pack(string.unpack(STAGES, reply, AT_STAGES + 64))
  r.read_stages, r.write_stages = {}, {}

  for i, name in ipairs(llm.STAGES) do
    r.read_stages[name], r.write_stages[name] = read[i], wrote[i]
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
-- **Qwen2 speaks ChatML**: each turn between `<|im_start|>` and
-- `<|im_end|>`, the two its tokenizer has as tokens of their own, and what
-- it is told first a `system` turn. Its answer ends with `<|im_end|>`,
-- which is its end-of-text token, so the server stops there by itself;
-- the stops are for the same words written as letters.
--
local FORMS = {
  phi2 = {
    before = function(b) return b end,
    question = function(q) return "Instruct: " .. q .. "\nOutput:" end,
    between = "\n",
    stops = { "\nInstruct:", "\nOutput:" },
  },
  qwen2 = {
    before = function(b) return "<|im_start|>system\n" .. b:gsub("\n$", "") .. "<|im_end|>\n" end,
    question = function(q) return "<|im_start|>user\n" .. q .. "<|im_end|>\n<|im_start|>assistant\n" end,
    between = "<|im_end|>\n",
    stops = { "<|im_end|>", "<|im_start|>" },
  },
}

function llm.form(arch)
  return FORMS[arch] or FORMS.phi2
end

-- **The architectures the LLM Kit has a forward pass for** - phi2 and
-- qwen2 (`llm_core.c`, `llm_open`, which refuses any other). One list, so
-- that what a window offers to get is what the server can load: another
-- architecture is a line here, a form above and a forward pass there.
llm.RUNS = { phi2 = true, qwen2 = true }

-- The text of a conversation - `before`, what the model is told first,
-- then `turns`, each `{ you = , xavier = }`, then `question` - and the
-- stops its answer ends at.
function llm.chat(arch, turns, question, before)
  local form = llm.form(arch)
  local parts = { before and before ~= "" and form.before(before) or "" }

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

-- **A seed for an answer drawn rather than taken** (`xavier.md`, *Answers
-- that make sense*): four bytes of the machine's entropy, never 0 - which
-- asks for the likeliest word every time. Given to `llm.ask` as `seed`,
-- and said by whoever asks, so the same answer can be drawn again.
function llm.seed()
  local bytes = sys.entropy(4)
  local seed = bytes and string.unpack("<I4", bytes) or 0

  return seed ~= 0 and seed or 1
end

local answer = {}
answer.__index = answer

-- A question, as the text the model is to continue - for a conversation,
-- all of it so far: the server keeps what its cache already holds of it and
-- reads only what is new (`llmproto.h`). `options.tokens` is the most to
-- answer with (128 when not said); `options.paragraph` ends the answer at
-- its first blank line; `options.stops`, up to two texts, end it where
-- they begin, and are not in it. Text longer than one message goes in a
-- region handed with the request. `options.seed`, from `llm.seed`, draws
-- the answer's words rather than taking the likeliest each time.
function llm.ask(text, options)
  options = options or {}
  local flags = options.paragraph and STOP_AT_BLANK_LINE or 0
  local fields = { tokens = options.tokens, stops = options.stops, seed = options.seed }
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

--------------------------------------------------------------------------
-- **Inside the model** (`llminside.h`; `docs/xavier.md`, *The LLM Monitor:
-- Inside*): the ring of the last tokens' passages, which the server fills
-- while somebody watches.
--
--   local ring = llm.inside()
--   for _, r in ipairs(ring:read()) do      -- what came since the last read
--     print(r.text, r.cands[1].text, r.cands[1].odds)
--     local layer = llm.inside_layer(r, 1)  -- { stream, fired, back = {}, share = {},
--                                           --   looks = { { back, share } ... }, sink }
--   end
--   ring:renew()                            -- on the window's clock: the lease
--
-- A record is kept as the one string it was read as, and its layers are
-- read out of it only when asked: 256 tokens of 32 layers of 64 numbers
-- each would be half a million values for the collector to walk.
--------------------------------------------------------------------------

local INSIDE_DATA, INSIDE_WRITTEN = 4096, 16
local RECORD_HEAD = "<I4I4I4I4I4I4I4I4c16"           -- seq .. rows_all, two spare, the text
local PASSAGE = "<I4i4I4I4I4I4"                       -- pos, token, n_layer, n_head, n_ff, n_cand
local AT_CAND_TEXT, AT_PASSAGE = 49, 209              -- 1-based, as string.unpack counts
local PASSAGE_LAYERS, PASSAGE_HEADS, CANDIDATES = 64, 32, 10
local LAYER = "<fI4" .. ("I2"):rep(2 * PASSAGE_HEADS + 7)
local LAYER_BYTES = 152
local AT_LAYERS = AT_PASSAGE + 104
local RECORD_BYTES = AT_LAYERS - 1 + LAYER_BYTES * PASSAGE_LAYERS   -- 10040, as llminside.h has it

llm.INSIDE_RECORDS = 256

local ring = {}
ring.__index = ring

local function record(raw)
  local seq, reading, batch, us, rows_gpu, rows_all, _, _, text = string.unpack(RECORD_HEAD, raw)
  local pos, token, n_layer, n_head, n_ff, n_cand = string.unpack(PASSAGE, raw, AT_PASSAGE)
  local r = {
    seq = seq, reading = reading ~= 0, batch = batch, us = us, rows_gpu = rows_gpu,
    rows_all = rows_all, text = cstring(text), pos = pos, token = token, n_layer = n_layer,
    n_head = n_head, n_ff = n_ff, cands = {}, raw = raw,
  }

  for k = 1, math.min(n_cand, CANDIDATES) do
    local id = string.unpack("<i4", raw, AT_PASSAGE + 24 + (k - 1) * 4)
    local odds = string.unpack("<f", raw, AT_PASSAGE + 64 + (k - 1) * 4)
    local said = string.unpack("c16", raw, AT_CAND_TEXT + (k - 1) * 16)

    r.cands[k] = { token = id, odds = odds, text = cstring(said) }
  end

  return r
end

-- The ring, and the server asked to fill it for a while.
function llm.inside()
  local r, cap = call(OP_INSIDE)

  if not r then return nil, cap end
  if not cap then return nil, "the LLM server sent no ring" end

  local bytes = sys.region_load32(cap, 4)
  local records = sys.region_load32(cap, 8)

  if sys.region_load32(cap, 0) ~= 0x49534e49 or bytes ~= RECORD_BYTES or records == 0 then
    sys.release(cap)
    return nil, "the LLM server's ring is not one this library reads"
  end

  local written = sys.region_load32(cap, INSIDE_WRITTEN)

  return setmetatable({ cap = cap, bytes = bytes, records = records,
                        seen = math.max(0, written - records), skipped = 0 }, ring)
end

-- What the ring holds now let go of: the next `read` gives only what
-- comes after this.
function ring:from_now()
  self.seen = sys.region_load32(self.cap, INSIDE_WRITTEN)
end

-- The lease renewed: the server goes on filling the ring for a while.
function ring:renew()
  local r, cap = call(OP_INSIDE)

  if cap then sys.release(cap) end
  return r ~= nil
end

-- The records written since the last read, oldest first; `self.skipped`
-- counts the ones written over before they were read.
function ring:read()
  local written = sys.region_load32(self.cap, INSIDE_WRITTEN)
  local out = {}

  if written - self.seen > self.records then
    self.skipped = self.skipped + (written - self.seen - self.records)
    self.seen = written - self.records
  end

  for seq = self.seen, written - 1 do
    local raw = sys.region_read(self.cap, INSIDE_DATA + (seq % self.records) * self.bytes, self.bytes)

    out[#out + 1] = record(raw)
  end

  -- Written over while it was read: the oldest ones read are not theirs.
  local after = sys.region_load32(self.cap, INSIDE_WRITTEN)
  local first = 1

  while first <= #out and after - out[first].seq > self.records do first = first + 1 end

  if first > 1 then
    self.skipped = self.skipped + first - 1
    out = table.move(out, first, #out, 1, {})
  end

  self.seen = written
  return out
end

-- Layer `l` (1-based) of a record: the residual stream's length after it,
-- the feed-forward neurons that fired, and each head's strongest look -
-- how many tokens back, and its share of the head's attention, 0 to 1.
-- And `looks`, what the layer weighed most past the first token - up to
-- three, the most weighed first, each how many back and its share
-- averaged over the heads - with `sink`, the first token's share
-- (`llm_core.h`, `struct llm_passage_layer`).
function llm.inside_layer(r, l)
  if l < 1 or l > math.min(r.n_layer, PASSAGE_LAYERS) then return nil end

  local v = table.pack(string.unpack(LAYER, r.raw, AT_LAYERS + (l - 1) * LAYER_BYTES))
  local heads = math.min(r.n_head, PASSAGE_HEADS)
  local layer = { stream = v[1], fired = v[2], back = {}, share = {} }

  for h = 1, heads do
    layer.back[h] = v[2 + h]
    layer.share[h] = v[2 + PASSAGE_HEADS + h] / 65535
  end

  local at = 2 + 2 * PASSAGE_HEADS
  layer.looks = {}
  for k = 1, 3 do
    local share = v[at + 3 + k]
    if share > 0 then layer.looks[#layer.looks + 1] = { back = v[at + k], share = share / 65535 } end
  end
  layer.sink = v[at + 7] / 65535

  return layer
end

-- **The answer forming, layer by layer** (`llminside.h`, LENS; `docs/
-- xavier.md`, *The Story page*): what each layer would have answered after
-- record `seq`'s token - the word the record after it holds, when the
-- answer wrote it - as { seq, layers = { { token, odds, text } ... } },
-- the first layer first. Refused while an answer runs ("busy"), and for a
-- record whose layers are not kept: the server keeps the last 64 tokens'
-- while the ring is filled.
local LENS_AT = INSIDE_DATA + llm.INSIDE_RECORDS * RECORD_BYTES

function ring:lens(seq)
  local r, why = call(OP_LENS, { record = seq })

  if not r then return nil, why end

  local raw = sys.region_read(self.cap, LENS_AT, 16 + 24 * PASSAGE_LAYERS)
  local got, n = string.unpack("<I4I4", raw)
  if got ~= seq then return nil, "the LLM server wrote another word's layers" end

  local out = { seq = seq, layers = {} }
  for l = 1, math.min(n, PASSAGE_LAYERS) do
    local token, odds, text = string.unpack("<i4fc16", raw, 17 + (l - 1) * 24)
    out.layers[l] = { token = token, odds = odds, text = cstring(text) }
  end
  return out
end

function ring:close()
  if self.cap then
    sys.release(self.cap)
    self.cap = nil
  end
end

return llm
