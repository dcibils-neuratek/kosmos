#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""**The LLM server, asked from inside Kosmos** (`docs/xavier.md` X3).

The tiny phi-2 (`tools/llm_tiny.py`) - the real architecture and phi-2's
own tokenizer at two layers 32 wide - is put in `/Home/Models` in all three
of its forms, Q8_0, Q4_0 and F16, and a program asks `/Running/llm` through
`llm.lua`, as Xavier does:

- **every answer is the kit's own, byte for byte**: each prompt's answer,
  read out of the region the server hands back, against what the same kit
  answers on the Mac (`tools/llm_answer.c`). The kit is held to llama.cpp
  by `test_llm`; this holds the server to the kit - its protocol, its
  region, its loop and the kit compiled freestanding for Kosmos. Bytes and
  not llama.cpp's tokens, because the tiny model's two best tokens are
  often a hundredth apart, inside the kit's own error against llama.cpp;
  the kit against itself on another processor differs in the fifth digit.
- **refusals in their own words**: a question with no model, a name that
  is a path, a model that is not there, a question longer than the model
  holds, and a second question while one is being answered;
- **a model kept**: a load refused leaves the one loaded where it was;
- **a stop**: an answer stopped is ended, short of its tokens;
- **a paragraph**: an answer asked to stop at a blank line that never
  meets one is not cut short - it is the kit's whole answer. The tiny
  model's random weights never write a blank line, so the stop itself is
  not seen here; phi-2's answers are where it is (`docs/xavier.md` X3);
- **the numbers**: STATS says the model's shape as the file has it, and
  tokens and times for the last answer;
- **and every region given back**: thirty answers asked and read, and the
  kernel's count of regions in use where it was before them.

Usage: run_llm.py IMAGE
"""

import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import scratch                                              # noqa: E402

LUA = os.path.join(ROOT, "build", "host", "lua")
KFS = os.path.join(HERE, "kfs.lua")
ANSWER = os.path.join(ROOT, "build", "host", "llm_answer")
MODELS = ["tiny-phi2.Q8_0.gguf", "tiny-phi2.Q4_0.gguf", "tiny-phi2.F16.gguf"]
TOKENS = 12
PARAGRAPH = "one\n\ntwo"

PROMPTS = [
    "def",
    "Instruct: What is the capital of France?\nOutput:",
    "def fibonacci(n):",
    "<|endoftext|>Next document.",
    "Café naïve über",
]

PROGRAM = r'''
local llm = use("/Kosmos/Libraries/llm.lua")
local function hex(s) return (s:gsub(".", function(c) return string.format("%02x", c:byte()) end)) end
local function unhex(h) return (h:gsub("..", function(x) return string.char(tonumber(x, 16)) end)) end

local s = llm.stats()
print("LLM-EMPTY", s and s.state)
local a, why = llm.ask("hi")
print("LLM-NOMODEL", a == nil, why)
local r, why = llm.load("../Keyring/keyring")
print("LLM-PATH", r == nil, why)
r, why = llm.load("missing.gguf")
print("LLM-MISSING", r == nil, why)

for _, model in ipairs({ MODELS }) do
  local r, why = llm.load(model)
  print("LLM-LOAD", model, r and r.state, why or "", r and r.title, r and r.arch,
        r and r.n_layer, r and r.n_embd, r and r.n_vocab, r and r.n_ctx, r and r.model_bytes)
  for _, h in ipairs({ PROMPTS }) do
    local a, why = llm.ask(unhex(h), { tokens = TOKENS })
    local text = a and a:all()
    local st = llm.stats() or {}
    print("LLM-ANSWER", model, h, a and hex(text) or ("refused " .. tostring(why)),
          st.prompt_tokens, st.answer_tokens, st.state)
  end
end

r, why = llm.load("missing.gguf")
s = llm.stats()
print("LLM-KEPT", r == nil, s.state, s.model)

a, why = llm.ask(string.rep("word ", 300))
print("LLM-TOOLONG", a == nil, why)

a = llm.ask("def", { tokens = 200 })
local b, why = llm.ask("def")
print("LLM-BUSY", b == nil, why)
a:stop()
local stopped = a:all()
s = llm.stats()
print("LLM-STOP", s.state, s.answer_tokens, #stopped)

a = llm.ask(unhex("PARAGRAPH"), { tokens = 200, paragraph = true })
local para = a:all()
print("LLM-PARAGRAPH", hex(para), llm.stats().answer_tokens)

local before = sys.info().regions_used
for i = 1, 30 do
  local a = llm.ask("def", { tokens = 2 })
  a:all()
end
local after = sys.info().regions_used
s = llm.stats()
print("LLM-REGIONS", before, after, s.answers)
print("LLM-STATS", s.prompt_us, s.answer_us, s.first_us, s.load_us)
print("LLM-DONE")
'''


def kfs(*args):
    return subprocess.run([LUA, KFS, *args], check=True, capture_output=True,
                          text=True, cwd=ROOT).stdout


def lua_list(items):
    return ", ".join('"%s"' % i for i in items)


def expected(model, prompts):
    """The kit's own answers on the Mac, by the prompt's hex."""
    return answers_of(model, TOKENS, prompts)


def answers_of(model, tokens, prompts, pieces=False):
    out = subprocess.run([ANSWER, os.path.join(ROOT, "build", "models", model), str(tokens),
                          *prompts], check=True, capture_output=True, text=True).stdout
    got = {}

    for line in out.splitlines():
        m = re.match(r"answer (\S+) : ?(\S*)$", line)
        if m:
            got[m.group(1)] = m.group(2).split(".") if pieces else m.group(2).replace(".", "")

    return got


def main():
    image = sys.argv[1] if len(sys.argv) > 1 else "build/x86_64/kosmos.elf"
    models = [os.path.join(ROOT, "build", "models", m) for m in MODELS]

    if not all(os.path.exists(m) for m in models):
        print("SKIP: no tiny phi-2 in build/models - `make llm-tiny`, docs/xavier.md X3")
        return 0

    work = scratch.directory("llm")
    disk = os.path.join(work, "disk.img")
    script = os.path.join(work, "llmtest.lua")
    hexes = [p.encode().hex() for p in PROMPTS]

    with open(script, "w") as f:
        f.write(PROGRAM.replace("{ MODELS }", "{ " + lua_list(MODELS) + " }")
                .replace("{ PROMPTS }", "{ " + lua_list(hexes) + " }")
                .replace("TOKENS", str(TOKENS))
                .replace('"PARAGRAPH"', '"%s"' % PARAGRAPH.encode().hex()))

    kfs("create", disk, "48", script + ":/Home/llmtest.lua",
        *[m + ":/Home/Models/" + os.path.basename(m) for m in models])

    os.environ["KOSMOS_DISK"] = disk
    import run_screenshot as R                              # noqa: E402

    fails, checks = [], 0

    def check(ok, what):
        nonlocal checks
        checks += 1
        if not ok:
            fails.append(what)

    guest = R.Guest(image, 240)

    try:
        guest.wait_for("kosmos>", "a prompt")
        # The prompt can come before the servers started after it: the LLM
        # server says when it is serving.
        guest.wait_for("llm: serving /Running/llm", "the LLM server")
        guest.type("run /Home/llmtest.lua")
        guest.wait_for("LLM-DONE", "the program's last line")
        time.sleep(0.3)
        guest._read_available()
    finally:
        guest.close()

    said = guest.seen

    def line(tag):
        m = re.search(r"^" + tag + r"\t(.*?)\r?$", said, re.M)
        return m.group(1).split("\t") if m else None

    def lines(tag):
        return [m.group(1).split("\t")
                for m in re.finditer(r"^" + tag + r"\t(.*?)\r?$", said, re.M)]

    check(line("LLM-EMPTY") == ["empty"], "before a load the server was not empty: %s" % line("LLM-EMPTY"))
    nm = line("LLM-NOMODEL")
    check(nm and nm[0] == "true" and "no model is loaded" in nm[1],
          "a question with no model was not refused as such: %s" % nm)
    p = line("LLM-PATH")
    check(p and p[0] == "true" and "not a path" in p[1], "a path for a name was not refused: %s" % p)
    p = line("LLM-MISSING")
    check(p and p[0] == "true" and "no missing.gguf in /Home/Models" in p[1],
          "a model not there was not refused by its name: %s" % p)

    answers = 0

    for model in MODELS:
        want = expected(model, hexes)
        loads = [l for l in lines("LLM-LOAD") if l[0] == model]
        size = os.path.getsize(os.path.join(ROOT, "build", "models", model))
        check(loads and loads[0][1:3] == ["ready", ""] and loads[0][3:10] ==
              ["tiny phi-2 for tests", "phi2", "2", "32", "51200", "256", str(size)],
              "%s did not load as the file is: %s" % (model, loads))

        for h in hexes:
            got = [l for l in lines("LLM-ANSWER") if l[0] == model and l[1] == h]
            ok = got and got[0][2] == want.get(h)
            check(ok, "%s answered %r with %s, not the kit's %s" %
                  (model, bytes.fromhex(h).decode(), got and got[0][2], want.get(h)))
            if ok:
                answers += 1
            if got:
                check(int(got[0][4]) <= TOKENS and got[0][5] == "ready",
                      "%s: %s tokens after asking for %d, or not ready after: %s" %
                      (model, got[0][4], TOKENS, got[0]))

    k = line("LLM-KEPT")
    check(k == ["true", "ready", MODELS[-1]], "a refused load did not keep the model: %s" % k)
    t = line("LLM-TOOLONG")
    check(t and t[0] == "true" and "longer than the model can hold" in t[1],
          "a question too long was not refused as such: %s" % t)
    b = line("LLM-BUSY")
    check(b and b[0] == "true" and "busy" in b[1], "a second question was not refused as busy: %s" % b)
    s = line("LLM-STOP")
    check(s and s[0] == "ready" and int(s[1]) < 200, "a stopped answer did not end short: %s" % s)
    pa = line("LLM-PARAGRAPH")
    whole = answers_of(MODELS[-1], 200, [PARAGRAPH.encode().hex()], pieces=True)
    whole = whole.get(PARAGRAPH.encode().hex(), [])
    check(not any(b"\n\n" in bytes.fromhex("".join(whole[:i + 1])) for i in range(len(whole))),
          "the kit's answer to the paragraph's prompt holds a blank line: the check means "
          "something else now")
    check(pa and pa[0] == "".join(whole),
          "an answer to stop at a blank line, with none in it, was not the kit's whole answer: "
          "%s against %s" % (pa and pa[0], "".join(whole)))
    r = line("LLM-REGIONS")
    check(r and r[0] == r[1], "thirty answers did not give their regions back: %s" % r)
    st = line("LLM-STATS")
    check(st and all(int(x) > 0 for x in st), "STATS gave a time of nothing: %s" % st)

    for f in fails:
        print("FAIL: " + f)

    if fails:
        print("FAIL: %d of %d checks\n%s" % (len(fails), checks, said[-3000:]))
        return 1

    print("PASS: %d checks - %d answers from /Running/llm the kit's own, byte for byte, in %s; "
          "refusals, a model kept, a stop, a paragraph and every region given back" %
          (checks, answers, ", ".join(MODELS)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
