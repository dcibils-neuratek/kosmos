#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""**The LLM Monitor and `llm`, counting what was asked** (`docs/xavier.md`,
*The LLM Monitor*).

A disk carries the tiny phi-2 (`tools/llm_tiny.py`) in `/Home/Models`, and:

- **before anything is asked**, `llm` says the server is up, with no
  answers, no tokens and nobody who has asked;
- **a question at the prompt** (`xavier`): `llm` names the asker by the
  kernel's name for its process - `xavier` - with one answer, and the tokens
  it read and wrote are the last answer's own;
- **a second question**: two answers, and its totals the sum of the two
  answers' - every token read and written counted once, and the server's
  totals the asker's;
- **the window**: LLM Monitor opens, and its first sample says what `llm`
  said, since both read the same reply.

Usage: run_llmmon.py IMAGE
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
MODEL = "tiny-phi2.Q4_0.gguf"

SINCE = re.compile(r"up (\d+) s; (\d+) answers, (\d+) tokens read, (\d+) written, (\d+) refused busy")
LAST = re.compile(r"(\d+) tokens in [\d.]+ s, question (\d+) tokens in")
ASKER = re.compile(r"^  (\S+)\s+(?:writing; |loading; )?(\d+) answers, (\d+) read, (\d+) written, "
                   r"(\d+) refused", re.M)


def main():
    image = sys.argv[1] if len(sys.argv) > 1 else "build/x86_64/kosmos.elf"
    model = os.path.join(ROOT, "build", "models", MODEL)

    if not os.path.exists(model):
        print("SKIP: no tiny phi-2 in build/models - `make llm-tiny`, docs/xavier.md X3")
        return 0

    work = scratch.directory("llmmon")
    disk = os.path.join(work, "disk.img")

    subprocess.run([LUA, KFS, "create", disk, "32", model + ":/Home/Models/" + MODEL],
                   check=True, capture_output=True, cwd=ROOT)

    import run_writeapp as WA                               # noqa: E402

    fails, checks = [], 0

    def check(ok, what):
        nonlocal checks
        checks += 1
        if not ok:
            fails.append(what)

    guest = WA.with_disk(image, disk)

    def said(prefix, since, seconds=60):
        deadline = time.monotonic() + seconds

        while time.monotonic() < deadline:
            guest._read_available()
            at = guest.seen.find(prefix, since)

            if at >= 0 and "\n" in guest.seen[at + len(prefix):]:
                return guest.seen[at + len(prefix):].split("\n", 1)[0].rstrip("\r")

            time.sleep(0.1)

        return None

    def at_prompt(command, seconds=120):
        """What `command` printed, up to the next prompt."""
        mark = len(guest.seen)
        guest.type(command)
        deadline = time.monotonic() + seconds

        while time.monotonic() < deadline:
            guest._read_available()
            end = guest.seen.find("kosmos> ", mark + len(command))

            if end >= 0:
                return guest.seen[mark:end]

            time.sleep(0.1)

        return guest.seen[mark:]

    def llm():
        out = at_prompt("llm", 30)
        since = SINCE.search(out)
        last = LAST.search(out)
        askers = {m.group(1): tuple(int(v) for v in m.groups()[1:]) for m in ASKER.finditer(out)}
        return out, since and tuple(int(v) for v in since.groups()), \
            last and (int(last.group(1)), int(last.group(2))), askers

    try:
        guest.wait_for("kosmos> ", "a prompt")
        guest.wait_for("llm: serving /Running/llm", "the LLM server")

        out0, since0, _, askers0 = llm()

        at_prompt("xavier --tokens 24 what is a kernel", 180)
        out1, since1, last1, askers1 = llm()

        at_prompt("xavier --tokens 16 and a process", 180)
        out2, since2, last2, askers2 = llm()

        mark = len(guest.seen)
        guest.type("wm llmmon:--tell")
        window = said("wm: window LLM Monitor at ", mark, 120)
        opened = said("llmmon: the LLM server, up ", mark, 60)
        told = said("llmmon: up ", mark, 30)

        # `KEEP_SHOT=file.png` keeps the screen, to be looked at by a person.
        if os.environ.get("KEEP_SHOT"):
            import run_screenshot as R                      # noqa: E402
            time.sleep(3)
            w, h, rgb = R.parse_ppm(guest.screendump())
            with open(os.environ["KEEP_SHOT"], "wb") as f:
                f.write(WA.png(w, h, rgb))
    finally:
        guest.close()

    check(since0 is not None and since0[1:] == (0, 0, 0, 0) and askers0 == {},
          "before a question, llm did not say nothing was asked: %r" % out0[-600:])

    check(since1 and last1 and since1[1] == 1 and since1[3] == last1[0] and since1[2] == last1[1],
          "after one question the server's totals are not its answer's: %r %r" % (since1, last1))
    check(askers1 == {"xavier": (1, last1[1], last1[0], 0)} if last1 else False,
          "after one question the asker is not xavier, with the answer's tokens: %r %r"
          % (askers1, out1[-600:]))

    if since1 and last1 and since2 and last2:
        want = (2, since1[2] + last2[1], since1[3] + last2[0])
        check(since2[1:4] == want,
              "after two questions the totals are not the two answers' sum: %r, %r" % (since2, want))
        check(askers2.get("xavier") == (2, want[1], want[2], 0) and len(askers2) == 1,
              "after two questions xavier's row is not the sum, alone: %r" % askers2)
        check(since2[0] >= since1[0], "the server's time went back: %r %r" % (since1, since2))
    else:
        check(False, "the second question was not counted: %r %r" % (since2, last2))

    check(window is not None, "LLM Monitor did not open")
    check(opened is not None, "LLM Monitor did not reach the server")

    if since2:
        check(told is not None and re.search(
                  r"model %s, read %d, written %d, answers 2, refused 0; xavier 2/0/%d/%d$"
                  % (re.escape(MODEL), since2[2], since2[3], since2[2], since2[3]), told),
              "LLM Monitor's sample is not what llm said: %r" % told)

    if fails:
        for f in fails:
            print("FAIL: " + f)
        print("the guest said:\n" + "\n".join(
            l for l in guest.seen.splitlines()
            if l.startswith(("llm", "  ", "since", "last", "xavier", "wm:", "lua")))[-3000:])
        return 1

    print("PASS: %d checks on the LLM Monitor - nothing asked, then xavier named by the kernel "
          "with each answer's %d and %d tokens written counted once, and the window saying "
          "what llm says" % (checks, last1[0], last2[0]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
