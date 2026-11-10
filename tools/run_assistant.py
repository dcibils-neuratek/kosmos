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
- **Settings**: Compare and Use the fastest (`xavier.md`, *Settings*);
- **a model for each conversation** (`xavier.html`, *A model for each
  conversation*): the header's chip lists both models on the disk; Q8_0
  chosen there is loaded, and a new conversation begins with it, its answer
  the kit's own in Q8_0, byte for byte;
- **the one from the Mac opened** from the list, its tricky answer read
  as one turn, its model - Q4_0 - loaded again, and **Delete** sends it to
  the Trash;
- **the first conversation opened again** with its four turns, now Q8_0's,
  and its answers still Q4_0's;
- **asked from anywhere** (`xavier.html`, *Asking from anywhere*): Super+X
  opens the box over the window, a question typed there is answered - the
  kit's own, byte for byte, told the day and the machine first, as every
  conversation is - and kept, and Tab opens it in Xavier's window.

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
OTHER = "tiny-phi2.Q8_0.gguf"
TOKENS = 40
STOPS = ["\nInstruct:", "\nOutput:"]

Q1 = "what is a kernel"
Q2 = "and a process"
LONG = " ".join(["go"] * 120)            # with two turns and the preamble held, more than 256 tokens;
                                         # 150 until the preamble grew by 30 (9 November)

SEEDED_ANSWER = "Hello.\nturn 1 2 3 4 5 6\nThat line is part of the answer."


def seeded(epoch):
    """A conversation as `conversations.lua` writes one, made here."""
    q, a = "hello there", SEEDED_ANSWER
    return ("kosmos conversation 1\ntitle \ncreated %d\nupdated %d\nmodel %s\nheld 30 256\n"
            "forgotten 0\nturn %d %d 12 1000 2000 300\n%s\n%s\n"
            % (epoch, epoch, MODEL, len(q.encode()), len(a.encode()), q, a))


def chain(texts, name=MODEL, tokens=TOKENS):
    """The kit's answers on the Mac to the texts the window asked, in order."""
    out = subprocess.run([ANSWER, "--chain", "--stops", ",".join(s.encode().hex() for s in STOPS),
                          os.path.join(ROOT, "build", "models", name), str(tokens),
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

    other = os.path.join(ROOT, "build", "models", OTHER)

    subprocess.run([LUA, KFS, "create", disk, "32", model + ":/Home/Models/" + MODEL,
                    other + ":/Home/Models/" + OTHER, old + ":/Home/Xavier/" + seed_name],
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

    asked, answered, seeds = [], [], []

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

            # For Stop, the pointer is on it before the question goes, and
            # pressed the moment it has gone: the tiny model reads and writes
            # its 40 tokens in under a second, less than `click` takes.
            if stop_it:
                guest.mouse_to(*R._to_tablet(wx + stop_at[0], wy + stop_at[1], sw, sh))
                time.sleep(0.3)

            guest.sendkey("ret")
            got = said("xavier: asked ", mark, 30)

            if stop_it:
                guest.mouse_button(True)
                time.sleep(0.05)
                guest.mouse_button(False)

            done = said("xavier: answered ", mark, 120)
            m = got and re.match(r"(\d+) bytes, drawn from (\d+): ([0-9a-f]+)$", got)
            d = done and re.match(r"(\d+) tokens, (\d+) of the conversation kept and (\d+) read, "
                                  r"(\d+) held: ([0-9a-f]*)$", done)
            asked.append(m.group(3) if m else None)
            seeds.append(int(m.group(2)) if m else 0)
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
        check("xavier: stopped" in fourth, "Stop was not pressed: %r"
              % " | ".join(l for l in fourth.splitlines() if l.startswith("xavier:"))[-700:])

        # **Settings** (`xavier.html`, *Settings*): opened from the header's
        # menu, Compare asking in each setting this machine has, and Use the
        # fastest keeping the winner, which the sheet then shows.
        settings_seen = compare_seen = kept_seen = reopened_settings = closed_settings = None
        compare_lines = []
        mark = len(guest.seen)
        click(p[9], p[10])
        menu = said("xavier: menu at ", mark, 10)
        mm = menu and re.match(r"(-?\d+),(-?\d+), rows of (\d+)", menu)

        if mm:
            mx, my, rh = (int(v) for v in mm.groups())
            click(mx + 30, my + 2 + rh // 2)
            settings_seen = said("xavier: settings, ", mark, 10)

        sp = settings_seen and [int(v) for v in re.findall(r"(\d+),(\d+)", settings_seen)[0] +
                                re.findall(r"(\d+),(\d+)", settings_seen)[1] +
                                re.findall(r"(\d+),(\d+)", settings_seen)[2]]

        if sp:
            mark = len(guest.seen)
            click(sp[0], sp[1])
            compare_seen = said("xavier: compared ", mark, 240)
            compare_lines = re.findall(r"xavier: compare (\d+) cores (\w+): read ([\d.]+), "
                                       r"wrote ([\d.]+) tokens a second", guest.seen[mark:])
            mark = len(guest.seen)
            click(sp[4], sp[5])
            kept_seen = said("xavier: the fastest kept, ", mark, 10)
            reopened_settings = said("xavier: settings, ", mark, 10)
            mark = len(guest.seen)
            click(sp[2], sp[3])
            closed_settings = said("xavier: settings closed", mark, 10)

        # **A model for each conversation**: the chip's menu, Q8_0 chosen
        # for this conversation and loaded, then a new conversation that
        # begins with it and is answered by it.
        models_menu = chose = loaded_other = new_with = None
        mark = len(guest.seen)
        click(p[11], p[12])
        models_menu = said("xavier: models menu at ", mark, 10)
        mm = models_menu and re.match(r"(-?\d+),(-?\d+), rows of (\d+): (.*)$", models_menu)

        if mm:
            mx, my, rh = int(mm.group(1)), int(mm.group(2)), int(mm.group(3))
            names = mm.group(4).split(", ")

            if OTHER in names:
                click(mx + 30, my + 2 + names.index(OTHER) * rh + rh // 2)
                chose = said("xavier: " + OTHER + " chosen for this conversation", mark, 10)
                loaded_other = said("xavier: ready, " + OTHER, mark, 60)

        mark = len(guest.seen)
        click(*new_at)
        new_with = said("xavier: opened a new conversation, 0 turns, ", mark, 10)
        other_asked = len(asked)
        ask(Q1)

        # The one from the Mac, opened from the list - Today's group, the new
        # conversation, the first one, This week's group, then it - its model
        # loaded again, and sent to the Trash.
        mark = len(guest.seen)
        click(list_x + 60, list_y + 30 + 2 * rowh + 30 + rowh // 2)
        opened_old = said("xavier: opened /Home/Xavier/" + seed_name, mark, 10)
        back_to_q4 = said("xavier: ready, " + MODEL, mark, 60)
        guest.sendkey("delete")
        trashed = said("xavier: to the Trash, /Home/Xavier/" + seed_name, mark, 10)
        said("turns", len(guest.seen), 5)
        time.sleep(0.5)
        guest._read_available()

        # The first conversation, opened again: Today's group, the new one,
        # then it.
        mark = len(guest.seen)
        click(list_x + 60, list_y + 30 + rowh + rowh // 2)
        first_again = said("xavier: opened /Home/Xavier/", mark, 10)

        # **From anywhere**: Super+X, a question, Return, then Tab.
        def chord(*names):
            for down, order in ((True, names), (False, tuple(reversed(names)))):
                guest._qmp("input-send-event", {"events": [
                    {"type": "key", "data": {"down": down, "key": {"type": "qcode", "data": n}}}
                    for n in order]})
                time.sleep(0.25)

        mark = len(guest.seen)
        chord("meta_l", "x")
        box_open = said("xavier box: open at ", mark, 30)
        box_ready = said("xavier box: ready, ", mark, 30)
        typed(Q1)
        guest.sendkey("ret")
        box_asked = said("xavier box: asked ", mark, 30)
        box_answered = said("xavier box: answered ", mark, 120)
        box_kept = said("xavier box: kept ", mark, 10)
        guest.sendkey("tab")
        box_carried = said("xavier box: to the window, ", mark, 10)
        carried_open = said("xavier: carried in ", len(guest.seen) if box_carried is None else
                            guest.seen.find("xavier box: to the window, ", mark), 60)
    finally:
        guest.close()

    seen = guest.seen
    first_kept = re.search(r"xavier: kept /Home/Xavier/\S+, 1 turns", seen)

    # The kit's answers to what was asked, in the same order, on the Mac:
    # the first four in Q4_0 across one conversation, the fifth alone in Q8_0.
    # Each drawn from the seed the window said, as the server drew it.
    texts = ["%d/%s" % (seeds[i], h) for i, h in enumerate(asked[:4]) if h]
    expect = chain(texts) if len(texts) == 4 else []
    other_expect = (chain(["%d/%s" % (seeds[4], asked[4])], OTHER)
                    if len(asked) == 5 and asked[4] else [])

    check(len(asked) == 5 and all(asked), "not every question was asked: %r" % asked)
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

    check(settings_seen is not None and settings_seen.startswith(
              "1 processors on 1 cores, cores 0, loops auto"),
          "Settings did not open from the menu saying the machine's one processor, Auto: %r"
          % settings_seen)

    if settings_seen:
        check(compare_seen == "2 settings" and [c[:2] for c in compare_lines] ==
              [("1", "base"), ("1", "plain")]
              and all(float(c[2]) > 0 and float(c[3]) > 0 for c in compare_lines),
              "Compare did not ask in base vectors and plain, each with a speed: %r %r"
              % (compare_seen, compare_lines))
        best = max(compare_lines, key=lambda c: float(c[3])) if compare_lines else None
        check(best and kept_seen == "1 cores %s" % best[1]
              and reopened_settings and reopened_settings.startswith(
                  "1 processors on 1 cores, cores 1, loops %s" % best[1]),
              "Use the fastest did not keep the fastest writer, %r: %r, then %r"
              % (best, kept_seen, reopened_settings))
        check(closed_settings is not None, "Done did not close Settings")

    check(models_menu is not None and models_menu.endswith(": %s, %s" % (MODEL, OTHER)),
          "the chip's menu did not list both models: %r" % models_menu)
    check(chose is not None and loaded_other is not None,
          "%s chosen from the chip was not loaded: %r %r" % (OTHER, chose, loaded_other))
    check(new_with is not None and new_with.startswith(OTHER),
          "a new conversation did not begin with the model chosen last: %r" % new_with)
    check(len(other_expect) == 1 and other_expect[0] and len(answered) == 5 and answered[4]
          and answered[4]["hex"] == other_expect[0]["hex"] and answered[4]["kept"] == 0,
          "the new conversation's answer is not the kit's in %s: %r against %r"
          % (OTHER, answered[4:], other_expect))
    check(back_to_q4 is not None,
          "opening the conversation from the Mac did not load its model, %s, again" % MODEL)
    check(first_again is not None and re.match(r"\S+, 4 turns, %s; answers by %s$"
                                               % (re.escape(OTHER), re.escape(MODEL)), first_again),
          "the first conversation did not open with four turns, now %s's, its answers %s's: %r"
          % (OTHER, MODEL, first_again))
    check(box_open is not None and box_ready == OTHER,
          "Super+X did not open the box on the model chosen last: %r %r" % (box_open, box_ready))
    bq = box_asked and re.match(r"\d+ bytes, drawn from (\d+): ([0-9a-f]+)$", box_asked)
    ba = box_answered and re.match(r"\d+ tokens, \d+ held: ([0-9a-f]*)$", box_answered)
    # The box is opened by a key, so with Xavier's own 512 tokens, not the
    # 40 the window was given: the tiny model's 256 end the answer first,
    # in the server as in the kit.
    box_expect = chain(["%s/%s" % (bq.group(1), bq.group(2))], OTHER, 512) if bq else []
    check(bq and bytes.fromhex(bq.group(2)).startswith(b"You are Xavier, a helpful assistant. "
                                                        b"You run on Kosmos")
          and b"Today is " in bytes.fromhex(bq.group(2))
          and b"in the language it was asked in." in bytes.fromhex(bq.group(2))
          and int(bq.group(1)) != 0,
          "the box did not tell the model what it is for, the machine and the day first, "
          "and draw its words: %r" % box_asked)
    check(ba and len(box_expect) == 1 and box_expect[0]
          and ba.group(1) == box_expect[0]["hex"],
          "the box's answer is not the kit's: %r against %r" % (box_answered, box_expect))
    check(box_kept is not None and box_carried is not None
          and box_kept.split(",")[0] == box_carried
          and carried_open == box_carried,
          "Tab did not open the box's conversation, kept, in the window: %r %r %r"
          % (box_kept, box_carried, carried_open))
    check(first_kept is not None, "the conversation was not kept after its first answer")
    check(opened_old is not None, "the conversation from the Mac did not open from the list")
    check(re.search(r"opened /Home/Xavier/%s, 1 turns" % re.escape(seed_name), seen),
          "the conversation from the Mac was not one turn")
    check(trashed is not None, "Delete did not send it to the Trash")

    if fails:
        for f in fails:
            print("FAIL: " + f)
        print("the guest said:\n" + "\n".join(l for l in seen.splitlines()
                                              if l.startswith(("xavier:", "llm:", "lua", "wm:")))[-3000:])
        return 1

    print("PASS: %d checks on Xavier's window - three answers the kit's own byte for byte "
          "across a conversation, a follow-up reading %d tokens with %d kept, the oldest turn "
          "forgotten, Stop, Settings, a model for each conversation - Q8_0's answer the kit's "
          "too - the conversation kept and opened again, and one sent to the Trash"
          % (checks, answered[1]["read"], answered[1]["kept"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
