-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- **The LLM server's settings at the prompt** (`docs/xavier.md`, *Settings*):
-- where the model runs - how many cores, which of the kit's loops, the GPU -
-- for every application that asks, as Xavier's Settings window shows them.
--
--   llm                      the settings, what this machine has, the last answer,
--                            and since the server started: who asked, and how much
--   llm set cores 4          a number, or auto
--   llm set loops avx2       auto, plain, base or avx2
--   llm set gpu read+write   where the GPU takes the products: off, read
--                            (a question's), write (an answer's), or both
--   llm inside               the last token's passage through the model, layer
--                            by layer, as the LLM Monitor's Inside page shows it
--   llm inside [--tokens n] <question>
--                            a question asked and every token's passage said:
--                            read or written, its time, what might follow
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
  print("       llm set gpu <off|read|write|read+write>")
  print("       llm inside [--tokens <n>] [<question>]")
end

local GPU_WORDS = { [0] = "off", [1] = "read questions", [2] = "write answers", [3] = "read and write" }
local GPU_BITS = { off = 0, read = 1, write = 2, ["read+write"] = 3 }

local function show(r)
  local s = r.settings
  local have = {}

  for _, name in ipairs({ "plain", "base", "avx2" }) do
    if r.loops_have[name] then have[#have + 1] = name end
  end

  print(("this machine:  %d processors on %d cores; loops %s; %s"):format(r.processors, r.cores,
        table.concat(have, ", "), r.gpu ~= "" and ("GPU " .. r.gpu) or "no GPU the server uses"))
  print(("settings:      cores %s, loops %s, GPU %s"):format(
        s.cores == 0 and ("auto (" .. r.cores .. ")") or tostring(s.cores),
        s.loops == "auto" and "auto" or (NAMES[s.loops] or s.loops), GPU_WORDS[s.gpu or 0] or "?"))

  if r.answers > 0 then
    print(("last answer:   %d cores, loops %s, GPU %s; %d tokens in %.2f s, question %d tokens in %.2f s")
          :format(r.ran.cores, NAMES[r.ran.loops] or r.ran.loops, GPU_WORDS[r.ran.gpu or 0] or "?",
                  r.answer_tokens, r.answer_us / 1e6, r.prompt_tokens, r.prompt_us / 1e6))
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

--------------------------------------------------------------------------
-- **Inside** (`docs/xavier.md`, *The LLM Monitor: Inside*): what the ring
-- of passages holds, said as lines.
--------------------------------------------------------------------------

local function quoted(text) return (("%q"):format(text):gsub("\\\n", "\\n")) end

-- A token on one line: where it is, read or written, how long its step
-- took and where the products ran, its stream from first layer to last,
-- and what might follow it.
local function token_line(r)
  local first, last = llm.inside_layer(r, 1), llm.inside_layer(r, r.n_layer)
  local next_words = {}

  for k = 1, math.min(3, #r.cands) do
    next_words[#next_words + 1] = ("%s %.0f%%"):format(quoted(r.cands[k].text), r.cands[k].odds * 100)
  end

  print(("%-8s %4d %-14s %s, %.1f ms, GPU %d%%; stream %.1f to %.1f%s")
        :format(r.reading and "read" or "written", r.pos, quoted(r.text),
                r.batch > 1 and (r.batch .. " together") or "alone", r.us / 1000,
                r.rows_all > 0 and r.rows_gpu * 100 // r.rows_all or 0,
                first and first.stream or 0, last and last.stream or 0,
                #next_words > 0 and ("; next " .. table.concat(next_words, ", ")) or ""))
end

-- A token's layers, one a line: its stream, the neurons that fired, and
-- the head that looked hardest at one token - which, and how far back.
local function layer_lines(r, text_at)
  print(("%s at %d, %d layers of %d heads and %d neurons:"):format(quoted(r.text), r.pos, r.n_layer,
                                                                   r.n_head, r.n_ff))

  for l = 1, r.n_layer do
    local L = llm.inside_layer(r, l)

    if not L then
      print(("  %2d  and on, not recorded: the ring holds 64 layers"):format(l))
      break
    end

    local best = 1

    for h = 2, #L.share do if L.share[h] > L.share[best] then best = h end end

    local at = r.pos - L.back[best]

    print(("  %2d  stream %7.2f  fired %5d (%2d%%)  head %2d looks %s, %d back, %.0f%%")
          :format(l, L.stream, L.fired, r.n_ff > 0 and L.fired * 100 // r.n_ff or 0, best,
                  L.back[best] == 0 and "at itself" or ("at " .. quoted(text_at[at] or "?")),
                  L.back[best], L.share[best] * 100))
  end
end

local function inside(rest)
  local tokens, question = 8, {}
  local i = 1

  while i <= #rest do
    if rest[i] == "--tokens" then
      tokens = tonumber(rest[i + 1])
      i = i + 2
      if not tokens then usage() return end
    else
      question[#question + 1] = rest[i]
      i = i + 1
    end
  end

  local ring, why = llm.inside()
  if not ring then print("llm: " .. tostring(why)) return end

  local text_at, seen = {}, {}

  local function take()
    for _, r in ipairs(ring:read()) do
      seen[#seen + 1] = r
      text_at[r.pos] = r.text
    end
  end

  if #question == 0 then
    -- What the ring already holds: the last token, layer by layer.
    take()
    ring:close()

    if #seen == 0 then
      print("llm: nothing recorded - the ring fills while the LLM Monitor's Inside page is open, "
            .. "or during llm inside <question>")
      return
    end

    layer_lines(seen[#seen], text_at)
    return
  end

  local s = llm.stats()
  if not s or s.state == "empty" then
    ring:close()
    print("llm: no model is loaded - ask Xavier something first")
    return
  end

  ring:from_now()

  local a, said = llm.ask(llm.form(s.arch).question(table.concat(question, " ")),
                          { tokens = tokens, paragraph = true })
  if not a then ring:close() print("llm: " .. tostring(said)) return end

  -- The lease renewed as the answer comes, and the ring read as it fills.
  while true do
    local text = a:wait()

    take()
    ring:renew()
    if not text then break end
  end

  take()
  ring:close()

  for _, r in ipairs(seen) do token_line(r) end
  local read = 0

  for _, r in ipairs(seen) do read = read + (r.reading and 1 or 0) end

  print(("%d tokens recorded, %d read and %d written%s"):format(#seen, read, #seen - read,
        ring.skipped > 0 and (", " .. ring.skipped .. " written over before they were read") or ""))
  if #seen > 0 then layer_lines(seen[#seen], text_at) end
end

if words[1] == "inside" then
  inside(table.move(words, 2, #words, 1, {}))
  return
end

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
elseif words[2] == "gpu" then
  change.gpu = GPU_BITS[words[3]]
  if not change.gpu then usage() return end
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
