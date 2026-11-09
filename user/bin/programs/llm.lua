-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- **The LLM server's settings at the prompt** (`docs/xavier.md`, *Settings*):
-- where the model runs - how many cores, which of the kit's loops, the GPU -
-- for every application that asks, as Xavier's Settings window shows them.
--
--   llm                      the settings, what this machine has, the last answer,
--                            and since the server started: who asked, and how much
--   llm set cores 4          a number, or auto
--   llm set loops avx2       auto, plain, base or avx2
--
-- In force from the next question, and kept by the server across restarts.
-- Every choice of loops gives the same answer to the bit; only the speed
-- differs.

local llm = use("/Kosmos/Libraries/llm.lua")

local NAMES = { plain = "plain (the reference)", base = "base (SSE2 or NEON)", avx2 = "AVX2" }

local function usage()
  print("usage: llm")
  print("       llm set cores <n|auto>")
  print("       llm set loops <auto|plain|base|avx2>")
end

local function show(r)
  local s = r.settings
  local have = {}

  for _, name in ipairs({ "plain", "base", "avx2" }) do
    if r.loops_have[name] then have[#have + 1] = name end
  end

  print(("this machine:  %d processors on %d cores; loops %s; %s"):format(r.processors, r.cores,
        table.concat(have, ", "), r.gpu ~= "" and ("GPU " .. r.gpu) or "no GPU the server uses"))
  print(("settings:      cores %s, loops %s"):format(
        s.cores == 0 and ("auto (" .. r.cores .. ")") or tostring(s.cores),
        s.loops == "auto" and "auto" or (NAMES[s.loops] or s.loops)))

  if r.answers > 0 then
    print(("last answer:   %d cores, loops %s; %d tokens in %.2f s, question %d tokens in %.2f s")
          :format(r.ran.cores, NAMES[r.ran.loops] or r.ran.loops, r.answer_tokens,
                  r.answer_us / 1e6, r.prompt_tokens, r.prompt_us / 1e6))
  end

  -- What the LLM Monitor shows, as lines (`xavier.md`, *The LLM Monitor*).
  print(("since start:   up %d s; %d answers, %d tokens read, %d written, %d refused busy")
        :format(r.up_us // 1000000, r.answers, r.read_tokens, r.written_tokens, r.refused))

  for _, a in ipairs(r.askers) do
    print(("  %-16s %s%d answers, %d read, %d written, %d refused, last asked at %d s")
          :format(a.name, a.answering and "writing; " or a.loading and "loading; " or "",
                  a.answers, a.read_tokens, a.written_tokens, a.refused, a.last_us // 1000000))
  end
end

local words = {}
for w in tostring(args or ""):gmatch("%S+") do words[#words + 1] = w end

if #words == 0 then
  local r, why = llm.settings()
  if not r then print("llm: " .. tostring(why)) return end
  show(r)
  return
end

if words[1] ~= "set" or #words ~= 3 then
  usage()
  return
end

local change = {}

if words[2] == "cores" then
  change.cores = words[3] == "auto" and 0 or tonumber(words[3])
  if not change.cores or change.cores < 0 or change.cores % 1 ~= 0 then usage() return end
elseif words[2] == "loops" then
  change.loops = words[3]
else
  usage()
  return
end

local r, why = llm.set(change)

if not r then
  print("llm: " .. tostring(why))
  return
end

show(r)
