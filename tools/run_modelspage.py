#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""**Xavier's Models page, used as a person would** (`docs/xavier.md`,
*Models*; drawn in `xavier.html`).

Hugging Face is the stand-in `run_models.py` makes - two HTTPS servers on
this Mac, the store sending slowly enough to press Pause, and cutting the
first download off a third of the way in. Xavier opens with the tiny
phi-2's Q4_0 on the disk, and:

- **the chip's menu** has "Get more in Models...", and it opens the page:
  the model on the disk listed, loaded, with Remove and no Load - it is in
  already;
- **a search**, typed and sent with Return, lists three repositories: one
  with the file the kit would take from it - its F16, since its Q4_0 is
  published only as Q4_K_M - one whose files the kit reads none of, and
  one of an architecture the kit has no forward pass for, phi3, whose
  files are never asked for - each of the two saying why, with no Get;
- **Get**: the download is cut off, and the page says so, with what came
  kept as a `.part` and **Go on**;
- **Go on** resumes it, and **Pause** stops it partway - further than the
  cut, short of the whole - again with Go on;
- **Go on** a second time: whole, checked, and listed with **Load**;
- **Load** makes it this conversation's, and the LLM server loads it;
- **Remove** on the Q4_0, no longer loaded, asks in a sheet first, and
  then it is gone from the folder;
- **Conversations** takes the window back to the conversation.

None of it waits in the window: each search and download is `models` in
a process of its own, and the page reads its report.

Usage: run_modelspage.py IMAGE
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
import run_tls                                              # noqa: E402
import run_models as M                                      # noqa: E402

LUA = os.path.join(ROOT, "build", "host", "lua")
KFS = os.path.join(HERE, "kfs.lua")
HERE_MODEL = "tiny-phi2.Q4_0.gguf"
GOOD = "tiny-phi2.F16.gguf"
REPO = "kosmos-test/tiny-phi2-GGUF"
KQUANTS = "kosmos-test/k-quants-GGUF"
PHI3 = "kosmos-test/phi3-GGUF"


def main():
    image = sys.argv[1] if len(sys.argv) > 1 else "build/x86_64/kosmos.elf"
    models_dir = os.path.join(ROOT, "build", "models")

    if not os.path.exists(os.path.join(models_dir, GOOD)):
        print("SKIP: no tiny phi-2 in build/models - `make llm-tiny`, docs/xavier.md X3")
        return 0

    work = scratch.directory("modelspage")
    run_tls.pki(work, M.NAME)

    def body(name):
        with open(os.path.join(models_dir, name), "rb") as f:
            return f.read()

    good, q4 = body(GOOD), body(HERE_MODEL)
    api_port, seen = M.stand_in(work, {
        REPO: {GOOD: (good, M.sha(good)), "tiny-phi2.Q4_K_M.gguf": (q4, M.sha(q4))},
        KQUANTS: {"tiny-phi2.Q4_K_M.gguf": (q4, M.sha(q4)), "tiny-phi2.Q5_K_S.gguf": (q4, M.sha(q4))},
        PHI3: {GOOD: (good, M.sha(good))},
    }, [{"id": REPO, "downloads": 42, "gguf": {"architecture": "phi2"}},
        {"id": KQUANTS, "downloads": 7, "gguf": {"architecture": "phi2"}},
        {"id": PHI3, "downloads": 3, "gguf": {"architecture": "phi3"}}], cut=GOOD, slow=True)

    disk = os.path.join(work, "disk.img")
    subprocess.run([LUA, KFS, "create", disk, "64",
                    M.authority(work) + ":/Home/Preferences/Authorities/test.der",
                    os.path.join(models_dir, HERE_MODEL) + ":/Home/Models/" + HERE_MODEL],
                   check=True, capture_output=True, cwd=ROOT)

    import run_screenshot as R                              # noqa: E402
    import run_writeapp as WA                               # noqa: E402

    board = "X86_ARGS" if R.machine(image) == "x86_64" else "QEMU_ARGS"
    saved = getattr(R, board)
    setattr(R, board, saved + ["-netdev", "user,id=net0",
                               "-device", R.device(image, "net") + ",netdev=net0"])

    try:
        guest = WA.with_disk(image, disk)
    finally:
        setattr(R, board, saved)

    fails, checks = [], 0

    def check(ok, what):
        nonlocal checks
        checks += 1
        if not ok:
            fails.append(what)

    def said(prefix, since, seconds=60):
        deadline = time.monotonic() + seconds

        while time.monotonic() < deadline:
            guest._read_available()
            at = guest.seen.find(prefix, since)

            if at >= 0 and "\n" in guest.seen[at + len(prefix):]:
                return guest.seen[at + len(prefix):].split("\n", 1)[0].rstrip("\r")

            time.sleep(0.1)

        return None

    def buttons_after(mark, want, seconds=30):
        """The page's buttons, as it last said them, once `want` is among them."""
        deadline = time.monotonic() + seconds
        last = None

        while time.monotonic() < deadline:
            guest._read_available()
            lines = re.findall(r"xavier: page buttons (.*)", guest.seen[mark:])

            if lines:
                last = lines[-1].rstrip("\r")
                if want in last:
                    break

            time.sleep(0.2)

        found = {}

        for one in (last or "").split("; "):
            m = re.match(r"(.+) (Load|Remove|Get|Go on|Pause|back) (\d+),(\d+)$", one)
            if m:
                found[(m.group(1), m.group(2))] = (int(m.group(3)), int(m.group(4)))

        return last, found

    error = None
    r = {}
    found_buttons = {}

    try:
        guest.wait_for("kosmos> ", "a prompt")
        guest.wait_for("net: an address from DHCP", "a lease")
        guest.wait_for("llm: serving /Running/llm", "the LLM server")

        mark = len(guest.seen)
        guest.type("wm assistant:--source https://10.0.2.2:%d --name %s" % (api_port, M.NAME))
        window = said("wm: window Xavier at ", mark, 120)
        places = said("xavier: places ", mark, 120)
        r["ready"] = said("xavier: ready, " + HERE_MODEL, mark, 90)

        wx, wy = (int(v) for v in re.match(r"(\d+),(\d+)", window).groups())
        p = [int(v) for v in re.findall(r"-?\d+", places)]
        sw, sh, _ = R.parse_ppm(guest.screendump())

        def click(x, y):
            guest.mouse_to(*R._to_tablet(wx + x, wy + y, sw, sh))
            time.sleep(0.3)
            guest.mouse_button(True)
            time.sleep(0.15)
            guest.mouse_button(False)
            time.sleep(0.4)

        # The chip's menu: the models, a separator a third of a row, the
        # folder's line, then "Get more in Models...".
        mark = len(guest.seen)
        click(p[11], p[12])
        menu = said("xavier: models menu at ", mark, 10)
        mm = re.match(r"(-?\d+),(-?\d+), rows of (\d+): (.*)$", menu)
        mx, my, rh = int(mm.group(1)), int(mm.group(2)), int(mm.group(3))
        n = len(mm.group(4).split(", "))
        click(mx + 30, my + 2 + n * rh + rh // 3 + rh + rh // 2)
        r["page"] = said("xavier: models page, ", mark, 10)
        r["first"], _ = buttons_after(mark, "Remove", 10)

        # A search, typed into the page's field, which has the keys.
        mark = len(guest.seen)
        for c in "tiny":
            guest.sendkey(c)
            time.sleep(0.05)
        guest.sendkey("ret")
        r["found"] = said("xavier: found ", mark, 60)
        _, b = buttons_after(mark, REPO + " Get", 10)
        found_buttons = dict(b)

        # Get: cut off a third of the way in.
        mark = len(guest.seen)
        click(*b[(REPO, "Get")])
        r["getting"] = said("xavier: getting %s from %s" % (GOOD, REPO), mark, 10)
        r["cut"] = said("xavier: %s failed" % GOOD, mark, 60)
        _, b = buttons_after(mark, "Go on", 10)

        # Go on, then Pause while it comes.
        mark = len(guest.seen)
        click(*b[("tiny-phi2 F16", "Go on")])
        _, b = buttons_after(mark, "Pause", 10)
        time.sleep(1.5)
        click(*b[("tiny-phi2 F16", "Pause")])
        r["paused"] = said("xavier: %s paused" % GOOD, mark, 30)
        _, b = buttons_after(mark, "Go on", 10)

        # Go on again: whole, checked, and Load offered.
        mark = len(guest.seen)
        click(*b[("tiny-phi2 F16", "Go on")])
        r["got"] = said("xavier: got " + GOOD, mark, 90)
        r["after_get"], b = buttons_after(mark, "tiny-phi2 F16 Load", 10)

        # `KEEP_SHOT=file.png` keeps the page as it is now, for a person.
        if os.environ.get("KEEP_SHOT"):
            time.sleep(1)
            w, h, rgb = R.parse_ppm(guest.screendump())
            with open(os.environ["KEEP_SHOT"], "wb") as f:
                f.write(WA.png(w, h, rgb))

        # Load it.
        mark = len(guest.seen)
        click(*b[("tiny-phi2 F16", "Load")])
        r["chosen"] = said("xavier: %s chosen for this conversation" % GOOD, mark, 10)
        r["loaded"] = said("xavier: ready, " + GOOD, mark, 60)
        r["after_load"], b = buttons_after(mark, "tiny-phi2 Q4_0 Load", 10)

        # Remove the Q4_0: asked, then gone.
        mark = len(guest.seen)
        click(*b[("tiny-phi2 Q4_0", "Remove")])
        asked = said("xavier: remove %s? " % HERE_MODEL, mark, 10)
        am = re.match(r"remove (\d+),(\d+) cancel (\d+),(\d+)", asked or "")
        r["asked"] = asked

        if am:
            click(int(am.group(1)), int(am.group(2)))

        r["removed"] = said("xavier: removed " + HERE_MODEL, mark, 10)
        r["after_rm"], b = buttons_after(mark, "tiny-phi2 F16 Remove", 10)

        # Back to the conversation.
        mark = len(guest.seen)
        click(*b[("Conversations", "back")])
        r["back"] = said("xavier: conversations again", mark, 10)
    except Exception as e:                  # noqa: BLE001 - said below
        error = "%s: %s" % (type(e).__name__, str(e).splitlines()[0] if str(e) else "")
    finally:
        guest.close()

    g = lambda k: r.get(k) or ""                            # noqa: E731

    check(error is None, "the steps stopped: %s" % error)
    check(r.get("ready") is not None, "Xavier did not load the model on the disk")
    check(g("page").startswith("1 in /Home/Models"), "the chip's menu did not open the page: %r" % g("page"))
    check("tiny-phi2 Q4_0 Remove" in g("first") and "Load" not in g("first"),
          "the loaded model was not listed with Remove and without Load: %r" % g("first"))
    check(("3 on Hugging Face for tiny: %s=%s, %s=none: its files are Q4_K_M, Q5_K_S; the LLM Kit "
           "reads Q4_0, Q8_0, F16 and F32, %s=none: a phi3 model: the LLM Kit has a forward pass "
           "for phi2, qwen2 only" % (REPO, GOOD, KQUANTS, PHI3)) == g("found"),
          "the search did not list the file the kit would take, and why not the others: %r" % g("found"))
    check(not any(PHI3 + "/tree" in a for a in seen["api"]) and (PHI3, "Get") not in found_buttons,
          "a model of an architecture the kit does not run was looked into or offered: %r %r"
          % (seen["api"], found_buttons))
    check((r.get("getting") is not None) and (r.get("cut") is not None) and seen["cut"],
          "Get, cut off, was not said: %r %r" % (g("getting"), g("cut")))

    ranges = [int(m.group(1)) for m in (re.search(r"Range: bytes=(\d+)-", h) for h in seen["store"]) if m]
    third = len(good) // 3
    check((r.get("paused") is not None) and len(ranges) == 2 and 0 < ranges[0] <= third < ranges[1] < len(good),
          "Go on and Pause did not resume from the cut and stop short of the whole: %r of %d, %r"
          % (ranges, len(good), g("paused")))
    check(re.match(r", %d bytes, checked" % len(good), g("got")),
          "the download was not whole and checked: %r" % g("got"))
    check("tiny-phi2 F16 Load" in g("after_get") and "Pause" not in g("after_get"),
          "the whole file was not listed with Load: %r" % g("after_get"))
    check((r.get("chosen") is not None) and (r.get("loaded") is not None),
          "Load did not make it this conversation's and load it: %r %r" % (g("chosen"), g("loaded")))
    check((r.get("asked") is not None) and (r.get("removed") is not None) and "tiny-phi2 Q4_0" not in g("after_rm"),
          "Remove did not ask, then take the Q4_0 from the disk: %r %r" % (g("asked"), g("after_rm")))
    check(r.get("back") is not None, "Conversations did not take the window back")

    if fails:
        for f in fails:
            print("FAIL: " + f)
        print("the API was asked: %r" % seen["api"])
        print("the guest said:\n" + "\n".join(l for l in guest.seen.splitlines()
                                              if l.startswith(("xavier", "wm:", "models", "lua")))[-3000:])
        return 1

    print("PASS: %d checks on Xavier's Models page - opened from the chip, a search of a stand-in "
          "Hugging Face naming the file the kit takes and why not the other, a Get cut off, gone "
          "on with from %d, paused at %d and finished, checked, loaded, and a model removed after "
          "asking" % (checks, ranges[0], ranges[1]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
