-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- **Xavier at the prompt** (`docs/xavier.md` X3): a question, answered by
-- the language model the LLM server holds, printed as the words arrive.
-- The window is X4; this is the same door, asked from a line.
--
--   xavier What is a microkernel?
--   xavier --model tiny-phi2.Q4_0.gguf Hello
--   xavier --tokens 32 Say something short
--   xavier --stats
--
-- With nothing loaded it loads a model from /Models (or /Home/Models on a
-- machine without that drive) first: the one
-- named, or phi-2's when it is there, or the first model in the folder.

local llm = use("/Kosmos/Libraries/llm.lua")

local MODELS = llm.MODELS

local function usage()
  print("usage: xavier [--model <file>] [--tokens <n>] [--raw] <question>")
  print("       xavier --stats")
  print("  a model is a .gguf file in " .. MODELS)
end

-- The words of the command line, and the options among them.
local words = {}
for w in tostring(args or ""):gmatch("%S+") do words[#words + 1] = w end

local model, tokens, raw, stats = nil, nil, false, false
local rest = {}
local i = 1
while i <= #words do
  local w = words[i]
  if w == "--model" then model = words[i + 1]; i = i + 2
  elseif w == "--tokens" then tokens = tonumber(words[i + 1]); i = i + 2
  elseif w == "--raw" then raw = true; i = i + 1
  elseif w == "--stats" then stats = true; i = i + 1
  else rest[#rest + 1] = w; i = i + 1 end
end
local question = table.concat(rest, " ")

local function ms(us) return string.format("%.1f ms", (us or 0) / 1000) end

local function show_stats(s)
  print("state:    " .. s.state)
  if s.model ~= "" then
    print("model:    " .. s.model .. (s.title ~= "" and ("  (" .. s.title .. ", " .. s.arch .. ")") or ""))
    print(string.format("shape:    %d layers, %d wide, %d tokens known, %d held",
                        s.n_layer, s.n_embd, s.n_vocab, s.n_ctx))
    print(string.format("size:     %.1f MB, loaded in %s", s.model_bytes / 1048576, ms(s.load_us)))
  end
  if s.answers > 0 or s.answer_tokens > 0 then
    local rate = s.answer_us > 0 and s.answer_tokens * 1e6 / s.answer_us or 0
    print(string.format("last:     %d tokens read in %s, %d written in %s (%.2f a second), first after %s",
                        s.prompt_tokens, ms(s.prompt_us), s.answer_tokens, ms(s.answer_us),
                        rate, ms(s.first_us)))
    print("answers:  " .. s.answers)
  end
end

if stats then
  local s, why = llm.stats()
  if not s then print("xavier: " .. why) return end
  show_stats(s)
  return
end

if question == "" then usage() return end

-- A model, when none is loaded or another was named.
local s, why = llm.stats()
if not s then print("xavier: " .. why) return end

if s.state == "empty" or (model and model ~= s.model) then
  local name = llm.choose(model)
  if not name then
    print("xavier: there is no model in " .. MODELS .. " - see docs/xavier.md X1")
    return
  end
  write("loading " .. name .. "...\n", "dim")
  local got, said = llm.load(name)
  if not got then print("xavier: " .. said) return end
  write(string.format("%s is in, %.1f MB in %s\n", name, got.model_bytes / 1048576,
                      ms(got.load_us)), "dim")
end

-- The model's own way of being asked, unless the question is given as it is.
local held = llm.stats()
local prompt = raw and question or llm.form(held and held.arch).question(question)

local a, said = llm.ask(prompt, { tokens = tokens or 128, paragraph = not raw })
if not a then print("xavier: " .. said) return end

while true do
  local text, err = a:wait()
  if not text then
    if err then print("\nxavier: " .. err) return end
    break
  end
  write(text)
end
write("\n")

local after = llm.stats()
if after and after.answer_us > 0 then
  write(string.format("%d tokens in %s, %.2f a second\n", after.answer_tokens, ms(after.answer_us),
                      after.answer_tokens * 1e6 / after.answer_us), "dim")
end
