#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""**The LLM server on Vulkan, asked from inside Kosmos** (`docs/xavier.md`
X12, V4).

`llm-vk.elf` - the LLM server with lavapipe linked in - is put where init
looks for it, `/Home/System/LLM`, with the tiny phi-2 (`tools/llm_tiny.py`)
in `/Home/Models` as F16 and Q4_0, and a program asks `/Running/llm`
through `llm.lua`, as Xavier does:

- **init starts the server from that image**, and says so;
- **a loaded model goes to the GPU**: the server says the model is on
  Vulkan, on llvmpipe;
- **every F16 answer is the kit's own on the processor, byte for byte**:
  each prompt's answer against what the kit answers on the Mac
  (`tools/llm_answer.c`). F16, because its products take floats on both
  sides; a Q4_0 product's input is quantised to Q8_0 on the processor and
  not on the GPU, so the tiny model's near ties fall the other way there
  (`test_llmvk`, `testing.md` 18.989) - a Q4_0 answer is held to being
  one, ready and as long as was asked;
- **every region given back** after five answers.

**The control**: the same disk booted with `opt/kosmos/llm=cpu`, where init
must start the server in its own image and no model may say it is on
Vulkan - and the F16 answers are still the kit's. So the first boot's lines
are the image's, and the answers are not passing because nothing changed.

Usage: run_llmvk.py IMAGE [APPS]
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
from run_llm import answers_of, kfs, lua_list               # noqa: E402

IMAGE = sys.argv[1] if len(sys.argv) > 1 else "build/x86_64/kosmos.elf"
APPS = sys.argv[2] if len(sys.argv) > 2 else os.path.join(ROOT, "build", "user-x86_64-test", "apps")
F16 = "tiny-phi2.F16.gguf"
Q4 = "tiny-phi2.Q4_0.gguf"
TOKENS = 12

PROMPTS = [
    "def",
    "Instruct: What is the capital of France?\nOutput:",
    "def fibonacci(n):",
    "Café naïve über",
]

PROGRAM = r'''
local llm = use("/Kosmos/Libraries/llm.lua")
local function hex(s) return (s:gsub(".", function(c) return string.format("%02x", c:byte()) end)) end
local function unhex(h) return (h:gsub("..", function(x) return string.char(tonumber(x, 16)) end)) end

for _, model in ipairs({ "F16", "Q4" }) do
  local r, why = llm.load(model)
  print("VK-LOAD", model, r and r.state, why or "")
  for i, h in ipairs({ PROMPTS }) do
    if model == "Q4" and i > 1 then break end
    local a, why = llm.ask(unhex(h), { tokens = TOKENS })
    local text = a and a:all()
    local st = llm.stats() or {}
    print("VK-ANSWER", model, h, a and hex(text) or ("refused " .. tostring(why)),
          st.answer_tokens, st.state)
  end
end

local before = sys.info().regions_used
for i = 1, 5 do llm.ask("def", { tokens = 2 }):all() end
print("VK-REGIONS", before, sys.info().regions_used)
print("VK-DONE")
'''


def boot(R, disk, words):
    """One boot of the disk with `words` on fw_cfg: everything it said."""
    board = R.X86_ARGS
    R.X86_ARGS = board + ["-smp", "4"] + words

    try:
        guest = R.Guest(IMAGE, 540)
    finally:
        R.X86_ARGS = board

    try:
        guest.wait_for("kosmos>", "a prompt")
        guest.wait_for("llm: serving /Running/llm", "the LLM server")
        guest.type("run /Home/llmvk.lua")
        guest.wait_for("VK-DONE", "the program's last line")
        time.sleep(0.3)
        guest._read_available()
    finally:
        guest.close()

    return guest.seen


def main():
    models = [os.path.join(ROOT, "build", "models", m) for m in (F16, Q4)]

    if not all(os.path.exists(m) for m in models):
        print("SKIP: no tiny phi-2 in build/models - `make llm-tiny`, docs/xavier.md X3")
        return 0

    built = os.path.join(APPS, "llm-vk.elf")

    if not os.path.exists(built):
        print("FAIL: no %s - `make ARCH=x86_64 apps` builds it" % built)
        return 1

    work = scratch.directory("llmvk")
    image = os.path.join(work, "llm-vk.elf")
    subprocess.run(["x86_64-elf-objcopy", "--strip-debug", built, image], check=True)

    disk = os.path.join(work, "disk.img")
    script = os.path.join(work, "llmvk.lua")
    hexes = [p.encode().hex() for p in PROMPTS]

    with open(script, "w") as f:
        f.write(PROGRAM.replace("{ PROMPTS }", "{ " + lua_list(hexes) + " }")
                .replace('"F16"', '"%s"' % F16).replace('"Q4"', '"%s"' % Q4)
                .replace("TOKENS", str(TOKENS)))

    size = os.path.getsize(image) // (1024 * 1024) + 48
    kfs("create", disk, str(size), script + ":/Home/llmvk.lua",
        image + ":/Home/System/LLM/llm-vk.elf",
        *[m + ":/Home/Models/" + os.path.basename(m) for m in models])

    # Before the import: `run_screenshot` reads the disk when it is loaded.
    os.environ["KOSMOS_DISK"] = disk
    import run_screenshot as R                              # noqa: E402

    want = answers_of(F16, TOKENS, hexes)
    fails, checks = [], 0

    def check(ok, what):
        nonlocal checks
        checks += 1
        if not ok:
            fails.append(what)

    def lines(said, tag):
        return [m.group(1).split("\t")
                for m in re.finditer(r"^" + tag + r"\t(.*?)\r?$", said, re.M)]

    def answers(said, run):
        """The F16 answers the kit's; the Q4_0 ones answers."""
        held = 0
        for l in lines(said, "VK-LOAD"):
            check(l[1:3] == ["ready", ""], "%s: %s did not load: %s" % (run, l[0], l))

        for h in hexes:
            got = [l for l in lines(said, "VK-ANSWER") if l[0] == F16 and l[1] == h]
            ok = got and got[0][2] == want.get(h)
            check(ok, "%s: F16 answered %r with %s, not the kit's %s" %
                  (run, bytes.fromhex(h).decode(), got and got[0][2], want.get(h)))
            held += 1 if ok else 0

            if h != hexes[0]:
                continue
            got = [l for l in lines(said, "VK-ANSWER") if l[0] == Q4 and l[1] == h]
            check(got and not got[0][2].startswith("refused") and len(got[0][2]) > 0
                  and int(got[0][3]) == TOKENS and got[0][4] == "ready",
                  "%s: Q4_0 did not answer %r with %d tokens and ready: %s" %
                  (run, bytes.fromhex(h).decode(), TOKENS, got))

        r = lines(said, "VK-REGIONS")
        check(r and r[0][0] == r[0][1], "%s: five answers did not give their regions back: %s" %
              (run, r))
        return held

    said = boot(R, disk, [])
    check("init: the LLM server from /Home/System/LLM/llm-vk.elf" in said,
          "init did not start the LLM server from its Vulkan image")
    check(re.search(r"llm: the model on Vulkan, llvmpipe", said) is not None,
          "a loaded model did not go to the GPU on llvmpipe")
    held = answers(said, "on Vulkan")

    cpu = boot(R, disk, ["-fw_cfg", "name=opt/kosmos/llm,string=cpu"])
    check("init: the LLM server from" not in cpu,
          "the control: init started the server from the Vulkan image with llm=cpu")
    check("the model on Vulkan" not in cpu,
          "the control: a model said it was on Vulkan in the system's own server")
    answers(cpu, "the control")

    for f in fails:
        print("FAIL: " + f)

    if fails:
        print("FAIL: %d of %d checks\n--- on Vulkan:\n%s\n--- the control:\n%s" %
              (len(fails), checks, said[-3000:], cpu[-2000:]))
        return 1

    print("PASS: %d checks - the LLM server started from llm-vk.elf, every model on Vulkan, "
          "%d F16 answers the kit's own byte for byte, Q4_0's answered, regions given back; "
          "and with llm=cpu the system's own server, on the processor, the same" %
          (checks, held))
    return 0


if __name__ == "__main__":
    sys.exit(main())
