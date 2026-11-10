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
  kernel's count of regions in use where it was before them;
- **a door shut answers**: on a second boot with no entropy, where
  `diskfs` cannot make its doors' words, a load is refused at once in the
  door's own words. It waited for good until 8 November.

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

-- Where the model runs (`xavier.md`, *Settings*): what the machine has,
-- a setting it cannot run refused, and the same answer in every setting.
llm.load("tiny-phi2.Q4_0.gguf")
local st = llm.settings()
print("LLM-SETTINGS", st.processors, st.cores, st.settings.cores, st.settings.loops,
      st.loops_have.plain and 1 or 0, st.loops_have.base and 1 or 0, st.loops_have.avx2 and 1 or 0)
local no, why = llm.set({ cores = st.processors + 1 })
print("LLM-SET-TOOMANY", no == nil, why, llm.settings().settings.cores)
no, why = llm.set({ loops = "fast" })
print("LLM-SET-NOLOOPS", no == nil, why)
for _, c in ipairs({ { 1, "plain" }, { 2, "base" }, { 0, "avx2" }, { 0, "auto" } }) do
  local ok, why = llm.set({ cores = c[1], loops = c[2] })
  local text = ""
  if ok then
    local a = llm.ask(unhex(SETPROMPT), { tokens = TOKENS })
    text = a and a:all() or ""
  end
  local s = llm.stats()
  print("LLM-RAN", c[1], c[2], ok and "ok" or why, s.ran.cores, s.ran.loops, hex(text))
end
local kept = fs.read(llm.MODELS .. "/llm.settings")
print("LLM-SETFILE", kept and #kept or 0)

-- The Story page's lens (`xavier.md`, *The Story page*, S3): a word's
-- layers read as though each were the last, its top layer the model's own
-- next word; a record never kept refused, and one written over since.
llm.set({ cores = 0, loops = "auto" })
local ring = llm.inside()
ring:from_now()
local nolens, why = ring:lens(999999)
print("LLM-LENS-NONE", nolens == nil, why)
a = llm.ask(unhex(SETPROMPT), { tokens = 4 })
a:all()
local recs = ring:read()
local last = recs[#recs]
local L, why = ring:lens(last.seq)
local top = L and L.layers[#L.layers]
print("LLM-LENS", #recs, L and #L.layers, top and top.token, last.cands[1].token,
      top and string.format("%.9g", top.odds), string.format("%.9g", last.cands[1].odds), why or "")
for _, w in ipairs({ "cat ", "dog ", "fish " }) do llm.ask(string.rep(w, 30), { tokens = 2 }):all() end
local old, why = ring:lens(last.seq)
print("LLM-LENS-GONE", old == nil, why)
ring:close()
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
                .replace('"PARAGRAPH"', '"%s"' % PARAGRAPH.encode().hex())
                .replace("SETPROMPT", '"%s"' % hexes[1]))

    shut_script = os.path.join(work, "llmshut.lua")

    with open(shut_script, "w") as f:
        f.write('local r, why = use("/Kosmos/Libraries/llm.lua").load("tiny-phi2.Q4_0.gguf")\n'
                'print("LLM-SHUT", r == nil, why)\n')

    kfs("create", disk, "48", script + ":/Home/llmtest.lua", shut_script + ":/Home/llmshut.lua",
        *[m + ":/Home/Models/" + os.path.basename(m) for m in models])

    os.environ["KOSMOS_DISK"] = disk
    import run_screenshot as R                              # noqa: E402

    fails, checks = [], 0

    def check(ok, what):
        nonlocal checks
        checks += 1
        if not ok:
            fails.append(what)

    # **Four cores** (`docs/xavier.md`, *Making it fast*): the x86 harness
    # boots one, and the server cuts its products across the cores there
    # are - so with one, the pool was never started and nothing here could
    # have told. The board's list is restored before the second machine.
    #
    # **And AVX2, on x86** (X2b): QEMU's default qemu64 has none, so the
    # kit's AVX2 loops would never be chosen here. `-cpu max` has it, and
    # every answer below is then theirs, held to the reference byte for
    # byte. `x86-xavier` keeps the default processor, so SSE2's are held
    # there. Rosetta has no AVX2, so a guest is the only place the Mac can.
    board = "X86_ARGS" if R.machine(image) == "x86_64" else "QEMU_ARGS"
    one_core = getattr(R, board)
    if "-smp" not in one_core:
        wider = one_core + ["-smp", "4"]
        if board == "X86_ARGS" and "-cpu" not in one_core:
            wider += ["-cpu", "max"]
        setattr(R, board, wider)

    try:
        guest = R.Guest(image, 240)
    finally:
        setattr(R, board, one_core)

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

    # **A door shut** (`roadmap.md`, *A disk door shut*): the machine with
    # no entropy, where diskfs cannot make its doors' words. A load is
    # refused in the door's words, at once - it used to wait for good.
    args, R.X86_ARGS = R.X86_ARGS, []
    for a in args:
        if a == "virtio-rng-pci":
            R.X86_ARGS.pop()            # its "-device"
        else:
            R.X86_ARGS.append(a)

    shut = R.Guest(image, 120)

    try:
        shut.wait_for("kosmos>", "a prompt with no entropy")
        shut.wait_for("llm: serving /Running/llm", "the LLM server with no entropy")
        shut.type("run /Home/llmshut.lua")
        shut.wait_for("LLM-SHUT\t", "an answer from a shut door")
        time.sleep(0.3)
        shut._read_available()
    finally:
        shut.close()
        R.X86_ARGS = args

    said_shut = shut.seen

    def line(tag):
        m = re.search(r"^" + tag + r"\t(.*?)\r?$", said, re.M)
        return m.group(1).split("\t") if m else None

    def lines(tag):
        return [m.group(1).split("\t")
                for m in re.finditer(r"^" + tag + r"\t(.*?)\r?$", said, re.M)]

    check(line("LLM-EMPTY") == ["empty"], "before a load the server was not empty: %s" % line("LLM-EMPTY"))
    # **Every core** (`docs/xavier.md`, *Making it fast*): the guest has
    # four, and the answers below - the kit's own on the Mac, made there on
    # one thread - were made with each product cut across all of them.
    check("llm: each product cut across 4 cores" in said,
          "the server did not cut its products across the guest's four cores")
    # And on x86 in AVX2's loops, which `-cpu max` above gives it.
    if R.machine(image) == "x86_64":
        check(re.search(r"llm: \S+ is loaded, its loops in AVX2", said) is not None,
              "the server did not choose AVX2's loops on a processor that has it")
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

    # **Settings** (`xavier.md`, *Settings*). The guest has four processors,
    # one a core; on x86 its `-cpu max` has AVX2, on ARM there is none.
    x86 = R.machine(image) == "x86_64"
    se = line("LLM-SETTINGS")
    check(se == ["4", "4", "0", "auto", "1", "1", "1" if x86 else "0"],
          "the settings did not say four processors on four cores, Auto, and the loops "
          "this processor has: %s" % se)
    se = line("LLM-SET-TOOMANY")
    check(se and se[0] == "true" and "cannot run those settings" in se[1] and se[2] == "0",
          "more cores than there are was not refused, or changed the settings: %s" % se)
    se = line("LLM-SET-NOLOOPS")
    check(se and se[0] == "true" and "no such loops" in se[1], "loops that do not exist were "
          "not refused: %s" % se)
    want = expected("tiny-phi2.Q4_0.gguf", [hexes[1]]).get(hexes[1])
    ran = {(l[0], l[1]): l[2:] for l in lines("LLM-RAN")}
    for (c, loops, cores, kind) in [("1", "plain", "1", "plain"), ("2", "base", "2", "base"),
                                    ("0", "avx2", "4", "avx2"), ("0", "auto", "4",
                                                                 "avx2" if x86 else "base")]:
        got = ran.get((c, loops))
        if loops == "avx2" and not x86:
            check(got and "cannot run those settings" in got[0],
                  "AVX2's loops were not refused on a processor without them: %s" % got)
            continue
        check(got and got[0:3] == ["ok", cores, kind] and got[3] == want,
              "cores %s, loops %s did not run on %s cores in %s's loops with the kit's answer: "
              "%s, the kit's %s" % (c, loops, cores, kind, got, want))
    se = line("LLM-SETFILE")
    check(se == ["24"], "the settings were not kept beside the models: %s" % se)

    # **The lens** (`xavier.md`, *The Story page*, S3).
    ln = line("LLM-LENS-NONE")
    check(ln and ln[0] == "true" and "not kept" in ln[1], "a lens of a record never kept was not "
          "refused as not kept: %s" % ln)
    ln = line("LLM-LENS")
    layers = {l[0]: l[5] for l in lines("LLM-LOAD") if len(l) > 5}
    check(ln and int(ln[0]) > 0 and ln[1] == layers.get("tiny-phi2.Q4_0.gguf")
          and ln[2] == ln[3] and ln[4] == ln[5],
          "the last word's lens through /Running/llm was not every layer, its top layer the model's "
          "likeliest next word at its odds: %s (layers %s)" % (ln, layers))
    ln = line("LLM-LENS-GONE")
    check(ln and ln[0] == "true" and "written over" in ln[1],
          "a word written over since was not refused: %s" % ln)

    m = re.search(r"^LLM-SHUT\t(.*?)\r?$", said_shut, re.M)
    sh = m.group(1).split("\t") if m else None
    check(sh and sh[0] == "true" and "door to the disk is shut" in sh[1],
          "with no entropy, a load was not refused in the shut door's words: %s" % sh)
    check("diskfs: no entropy for a door; it is shut, and refuses" in said_shut,
          "with no entropy, diskfs did not say its doors are shut and refusing")

    for f in fails:
        print("FAIL: " + f)

    if fails:
        print("FAIL: %d of %d checks\n%s" % (len(fails), checks, said[-3000:]))
        return 1

    print("PASS: %d checks - %d answers from /Running/llm the kit's own, byte for byte, in %s; "
          "refusals, a model kept, a stop, a paragraph, every region given back, and a "
          "shut door refusing" %
          (checks, answers, ", ".join(MODELS)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
