#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""**Xavier's window, used as a person would** (`docs/xavier.md` X4).

A disk carries the tiny phi-2 (`tools/llm_tiny.py`) in `/Home/Models` and a
conversation made on the Mac in `/Home/Xavier` - one whose answer has a
line in it that looks like the file's own `turn` header. Xavier is opened
with answers of 40 tokens, so the tiny model's 256 hold a follow-up, and:

- **it opens on what is there**: one conversation listed, the model loaded
  without the window waiting for it, and then ready;
- **a question** typed and sent with Return: its answer, read out of the
  answer's region as it grows, is the kit's own on the Mac, byte for byte
  (`tools/llm_answer.c --chain`, which holds the cache across a
  conversation through the same `llm_prompt` and `llm_feed`);
- **a follow-up** reads only its own words: the server keeps the rest of
  the conversation in its cache, and the answer is still the kit's;
- **a question too long for what is held**: the oldest turn is forgotten,
  the rest asked, and the answer the kit's for what was asked;
- **Stop**, pressed while a question is being read, ends the answer short;
- **the conversation kept**: after each answer it is a file in
  `/Home/Xavier`, and opened again from the list it has its four turns;
- **the one from the Mac opened** from the list, its tricky answer read
  as one turn, and **Delete** sends it to the Trash.

Usage: run_assistant.py IMAGE
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
MODEL = "tiny-phi2.Q4_0.gguf"
TOKENS = 40
STOPS = ["\nInstruct:", "\nOutput:"]

Q1 = "what is a kernel"
Q2 = "and a process"
LONG = " ".join(["go"] * 175)            # with two turns held, more than 256 tokens

SEEDED_ANSWER = "Hello.\nturn 1 2 3 4 5 6\nThat line is part of the answer."


def seeded(epoch):
    """A conversation as `conversations.lua` writes one, made here."""
    q, a = "hello there", SEEDED_ANSWER
    return ("kosmos conversation 1\ntitle \ncreated %d\nupdated %d\nmodel %s\nheld 30 256\n"
            "forgotten 0\nturn %d %d 12 1000 2000 300\n%s\n%s\n"
            % (epoch, epoch, MODEL, len(q.encode()), len(a.encode()), q, a))


def chain(texts):
    """The kit's answers on the Mac to the texts the window asked, in order."""
    out = subprocess.run([ANSWER, "--chain", "--stops", ",".join(s.encode().hex() for s in STOPS),
                          os.path.join(ROOT, "build", "models", MODEL), str(TOKENS),
                          *texts], check=True, capture_output=True, text=True).stdout
    got = []

    for line in out.splitlines():
        m = re.match(r"answer \S+ : (\S*) kept (\d+) read (\d+) held (\d+)$", line)
        if m:
            got.append({"hex": m.group(1), "kept": int(m.group(2)), "read": int(m.group(3)),
                        "held": int(m.group(4))})
        elif line.startswith("answer "):
            got.append(None)

    return got


def main():
    image = sys.argv[1] if len(sys.argv) > 1 else "build/x86_64/kosmos.elf"
    model = os.path.join(ROOT, "build", "models", MODEL)

    if not os.path.exists(model):
        print("SKIP: no tiny phi-2 in build/models - `make llm-tiny`, docs/xavier.md X3")
        return 0

    work = scratch.directory("assistant")
    disk = os.path.join(work, "disk.img")
    old = os.path.join(work, "old.conversation")
    seed_name = "2026-11-01-100000.conversation"

    with open(old, "w") as f:
        f.write(seeded(int(time.time()) - 3 * 86400))

    subprocess.run([LUA, KFS, "create", disk, "32", model + ":/Home/Models/" + MODEL,
                    old + ":/Home/Xavier/" + seed_name],
                   check=True, capture_output=True, cwd=ROOT)

    import run_screenshot as R                              # noqa: E402
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

    asked, answered = [], []

    try:
        guest.wait_for("kosmos> ", "a prompt")
        guest.wait_for("llm: serving /Running/llm", "the LLM server")

        mark = len(guest.seen)
        guest.type("wm assistant:--tokens %d" % TOKENS)
        window = said("wm: window Xavier at ", mark, 120)
        places = said("xavier: places ", mark, 120)

        if window is None or places is None:
            print("FAIL: Xavier did not open:\n" + guest.seen[mark:][-1500:])
            return 1

        check(re.search(r"xavier: 1 conversations in /Home/Xavier", guest.seen[mark:]),
              "the conversation on the disk was not listed")

        wx, wy = (int(v) for v in re.match(r"(\d+),(\d+)", window).groups())
        p = [int(v) for v in re.findall(r"-?\d+", places)]
        field_at, stop_at, new_at = (p[0], p[1]), (p[2], p[3]), (p[4], p[5])
        list_x, list_y, rowh = p[6], p[7], p[8]

        check(said("xavier: loading " + MODEL, mark, 30) is not None,
              "the model was not loaded by the window")
        check(said("xavier: ready, " + MODEL, mark, 90) is not None,
              "the model never became ready")

        sw, sh, _ = R.parse_ppm(guest.screendump())

        def click(x, y):
            guest.mouse_to(*R._to_tablet(wx + x, wy + y, sw, sh))
            time.sleep(0.3)
            guest.mouse_button(True)
            time.sleep(0.15)
            guest.mouse_button(False)
            time.sleep(0.4)

        def typed(text, pause=0.03):
            for c in text:
                guest.sendkey("spc" if c == " " else c)
                time.sleep(pause)

        def ask(question, stop_it=False):
            mark = len(guest.seen)
            click(*field_at)
            typed(question)
            guest.sendkey("ret")
            got = said("xavier: asked ", mark, 30)

            if stop_it:
                click(*stop_at)

            done = said("xavier: answered ", mark, 120)
            m = got and re.match(r"(\d+) bytes: ([0-9a-f]+)$", got)
            d = done and re.match(r"(\d+) tokens, (\d+) of the conversation kept and (\d+) read, "
                                  r"(\d+) held: ([0-9a-f]*)$", done)
            asked.append(m.group(2) if m else None)
            answered.append({"tokens": int(d.group(1)), "kept": int(d.group(2)),
                             "read": int(d.group(3)), "held": int(d.group(4)),
                             "hex": d.group(5)} if d else None)
            return guest.seen[mark:]

        # A new conversation, and three questions in it.
        mark = len(guest.seen)
        click(*new_at)
        check(said("xavier: opened a new conversation, 0 turns", mark, 10) is not None,
              "New conversation did not open one")

        ask(Q1)
        ask(Q2)
        third = ask(LONG)
        check("the conversation did not fit; 1 turns forgotten" in third,
              "the long question did not forget the oldest turn")
        fourth = ask(LONG + " go", stop_it=True)
        check("xavier: stopped" in fourth, "Stop was not pressed")

        # The one from the Mac, opened from the list - Today's group, this
        # conversation, This week's group, then it - and sent to the Trash.
        mark = len(guest.seen)
        click(list_x + 60, list_y + 30 + rowh + 30 + rowh // 2)
        opened_old = said("xavier: opened /Home/Xavier/" + seed_name, mark, 10)
        guest.sendkey("delete")
        trashed = said("xavier: to the Trash, /Home/Xavier/" + seed_name, mark, 10)
        said("turns", len(guest.seen), 5)
        time.sleep(0.5)
        guest._read_available()
    finally:
        guest.close()

    seen = guest.seen
    first_kept = re.search(r"xavier: kept /Home/Xavier/\S+, 1 turns", seen)
    after = re.search(r"xavier: to the Trash, [^\n]*\n(?:[^\n]*\n)*?xavier: opened "
                      r"/Home/Xavier/\S+, (\d+) turns", seen)
    reopened = after.group(1) if after else None

    # The kit's answers to what was asked, in the same order, on the Mac.
    texts = [h for h in asked if h]
    expect = chain(texts) if len(texts) == len(asked) else []

    check(len(asked) == 4 and all(asked), "not every question was asked: %r" % asked)
    check(all(answered), "not every question was answered")

    if len(expect) == 4 and all(answered) and all(expect):
        for i in range(3):
            check(answered[i]["hex"] == expect[i]["hex"],
                  "answer %d is not the kit's: %s against %s" % (i + 1, answered[i]["hex"],
                                                                expect[i]["hex"]))
            check(answered[i]["kept"] == expect[i]["kept"]
                  and answered[i]["read"] == expect[i]["read"],
                  "answer %d kept %d and read %d; the kit, %d and %d" % (
                      i + 1, answered[i]["kept"], answered[i]["read"], expect[i]["kept"],
                      expect[i]["read"]))

        check(answered[0]["kept"] == 0, "the first question kept something: %r" % answered[0])
        check(answered[1]["kept"] > 0 and answered[1]["read"] < 12,
              "the follow-up did not keep the conversation: %r" % answered[1])
        check(bytes.fromhex(asked[1]).startswith(bytes.fromhex(asked[0])),
              "the follow-up did not ask the conversation whole")
        check(not bytes.fromhex(asked[2]).startswith(bytes.fromhex(asked[0])),
              "the long question still carried the first turn")
        check(answered[3]["tokens"] < TOKENS
              and expect[3]["hex"].startswith(answered[3]["hex"]),
              "the stopped answer was not cut short of the kit's: %r" % answered[3])

    check(first_kept is not None, "the conversation was not kept after its first answer")
    check(opened_old is not None, "the conversation from the Mac did not open from the list")
    check(re.search(r"opened /Home/Xavier/%s, 1 turns" % re.escape(seed_name), seen),
          "the conversation from the Mac was not one turn")
    check(trashed is not None, "Delete did not send it to the Trash")
    check(reopened == "4",
          "the conversation kept did not open again with its four turns: %r" % reopened)

    if fails:
        for f in fails:
            print("FAIL: " + f)
        print("the guest said:\n" + "\n".join(l for l in seen.splitlines()
                                              if l.startswith(("xavier:", "llm:", "lua", "wm:")))[-3000:])
        return 1

    print("PASS: %d checks on Xavier's window - three answers the kit's own byte for byte "
          "across a conversation, a follow-up reading %d tokens with %d kept, the oldest turn "
          "forgotten, Stop, the conversation kept and opened again, and one sent to the Trash"
          % (checks, answered[1]["read"], answered[1]["kept"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
