#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""How Linux's desktop applications feel on a Kosmos machine, measured
(`docs/linux.md` L6a; Diego, 15 October 2026: "lets make the measurements
and understand if its a real thing we might be using graphic apps or just
text apps on linux").

Drives a machine running Kosmos - the M700 - from this Mac: Telnet to start
Linux and read what it says, VNC for the keys and the pointer. The Linux
window, opened with `--tell`, says each second how many frames Linux handed
it, the longest wait between two of them while it drew, and the time from
each key to the next frame (`linux.lua`, the meter). This runs a fixed
sequence in Epiphany and writes the numbers down:

  * **pages**: each address typed and Return pressed, and the time until
    Linux stopped drawing - the page loaded and laid out, near enough;
  * **scrolling**: the wheel turned ten times a second for five seconds on
    each page, and the frames a second and the longest gap while it did;
  * **typing**: twenty letters into the address bar, a third of a second
    apart, and each key's time to the screen;
  * **where the time went**: each process's share of the machine while a
    page loaded and while it scrolled, and each core's.

It opens Linux with `--tell` and leaves it open; a run that finds it open
uses it, which is right only when this opened it - the meter speaks only
with `--tell`.

Usage: linuxbench.py [ADDRESS] [--out FILE]
       ADDRESS defaults to 192.168.1.40, the M700.
"""

import datetime
import os
import re
import statistics
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import kosmos_telnet as K                                   # noqa: E402
import kosmos_vnc as V                                      # noqa: E402

PAGES = ["example.com", "en.wikipedia.org/wiki/Operating_system", "www.bbc.com/news"]

# Each process's share of the machine over some seconds, and each core's
# busy share - run on the machine, its lines read back here.
LOAD = r'''
local share = use("/Kosmos/Libraries/procshare.lua")
local seconds = tonumber(args) or 5
local state, cores0 = {}, sys.cpuload() or {}
share.rows(state, sys.processes(), fs.read("/Devices/kernel"))
sys.sleep(seconds * 1000)
local rows = share.rows(state, sys.processes(), fs.read("/Devices/kernel"))
local n = #cores0
for _, r in ipairs(rows) do
  local name = r.kernel and "nebula" or tostring(r.process.name)
  if r.pct * n >= 2 then print(("LOAD %s %d"):format(name, r.pct * n)) end
end
local cores1 = sys.cpuload() or {}
for c = 1, n do
  local a, b = cores0[c], cores1[c]
  if a and b then
    local busy, all = b.busy - a.busy, (b.busy + b.idle) - (a.busy + a.idle)
    print(("CORE %d %d"):format(c, all > 0 and busy * 100 // all or 0))
  end
end
'''

STAMP = re.compile(r"^\[(\d+\.\d+)\] (.*)$")


def now_on(session):
    """The machine's own time, from the newest line of its log."""
    for line in reversed(session.run("log 3").decode(errors="replace").splitlines()):
        m = STAMP.match(line.strip())
        if m:
            return float(m.group(1))
    return None


def meters_since(session, since):
    """The meter's lines after `since`: (time, fps, gap ms, [key ms])."""
    out = []

    for line in session.run("log meter").decode(errors="replace").splitlines():
        m = STAMP.match(line.strip())
        if not m or float(m.group(1)) <= since:
            continue
        f = re.search(r"linux: meter fps=(\d+) gap=(\d+) keys=([\d,]*)", m.group(2))
        if f:
            keys = [int(k) for k in f.group(3).split(",") if k]
            out.append((float(m.group(1)), int(f.group(1)), int(f.group(2)), keys))

    return out


def load(session, seconds):
    said = session.run("/Temporary/linuxload.lua %d" % seconds).decode(errors="replace")
    procs = [(m.group(1), int(m.group(2))) for m in re.finditer(r"^LOAD (\S+) (\d+)", said, re.M)]
    cores = [int(m.group(2)) for m in re.finditer(r"^CORE (\d+) (\d+)", said, re.M)]
    return sorted(procs, key=lambda p: -p[1]), cores


def wheel(viewer, x, y, notches, every):
    for _ in range(notches):
        viewer.pointer(x, y, 16)
        viewer.pointer(x, y, 0)
        time.sleep(every)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    address = args[0] if args else "192.168.1.40"
    out = None

    if "--out" in sys.argv:
        out = sys.argv[sys.argv.index("--out") + 1]

    session = K.Session(address)
    session.put(LOAD.encode(), "/Temporary/linuxload.lua")

    # Running already, from an earlier run of this - its meter is on, and its
    # picture is where it last said; otherwise opened, with `--tell`.
    running = "sandbox-linux" in session.run("ls /Running").decode(errors="replace")
    started = 0.0 if running else now_on(session)

    if not running:
        session.run("open linux --tell")

    picture = None

    for _ in range(3 if running else 120):
        said = session.run("log linux: picture").decode(errors="replace")
        found = [m for m in re.finditer(r"^\[(\d+\.\d+)\] linux: picture at (\d+),(\d+), (\d+)x(\d+)",
                                        said, re.M) if float(m.group(1)) > started]
        if found:
            picture = tuple(int(v) for v in found[-1].groups()[1:])
            break
        time.sleep(2)

    # Remembered from the run that opened it: the meter's lines push "picture
    # at" out of the machine's log within minutes.
    where = os.path.join("build", "linuxbench", "where.txt")

    if picture:
        os.makedirs(os.path.dirname(where), exist_ok=True)
        with open(where, "w") as f:
            f.write("%d %d %d %d\n" % picture)
    elif running and os.path.exists(where):
        picture = tuple(int(v) for v in open(where).read().split())

    if not picture:
        print("linuxbench: Linux's picture never arrived")
        return 1

    px, py, pw, ph = picture

    # Epiphany up and settled: until a second with no frames, after the
    # session has started.
    for _ in range(0 if running else 90):
        time.sleep(2)
        if "kosmos-session: cage" in session.run("log kosmos-session").decode(errors="replace"):
            break

    if not running:
        time.sleep(20)

    viewer = V.Viewer(address)
    bar = (px + pw // 2, py + 22)
    middle = (px + pw // 2, py + ph // 2)
    report = []

    def click(at):
        viewer.pointer(at[0], at[1], 0)
        time.sleep(0.1)
        viewer.pointer(at[0], at[1], 1)
        viewer.pointer(at[0], at[1], 0)
        time.sleep(0.3)

    for page in PAGES:
        click(bar)
        click(bar)
        V.step(viewer, "key ctrl+a")
        V.step(viewer, "type " + page)
        time.sleep(0.5)
        mark = now_on(session)
        V.step(viewer, "key Return")
        procs, cores = load(session, 12)
        time.sleep(4)
        seen = meters_since(session, mark)
        # Loaded: the last second Linux drew in earnest - five frames or
        # more. A text cursor blinking draws two a second for ever, and the
        # first version counted those: every page "loaded" when the window
        # it was measured over ended, the longest gap the blink's 500 ms.
        end = None
        for m in seen:
            if m[1] >= 5:
                end = m[0]
        took = (end - mark) if end else None
        active = [m for m in seen if m[1] >= 5]

        click(middle)
        mark = now_on(session)
        # The wheel turned while the machine's shares are taken, not after.
        turning = threading.Thread(target=wheel, args=(viewer, middle[0], middle[1], 50, 0.1))
        turning.start()
        sprocs, scores = load(session, 4)
        turning.join()
        time.sleep(2)
        scrolled = [m for m in meters_since(session, mark) if m[1] > 0]

        report.append({
            "page": page, "load_s": took,
            "load_fps": statistics.mean(m[1] for m in active) if active else 0,
            "load_gap": max((m[2] for m in active), default=0),
            "load_procs": procs, "load_cores": cores,
            "scroll_fps": [m[1] for m in scrolled],
            "scroll_gap": max((m[2] for m in scrolled), default=0),
            "scroll_procs": sprocs, "scroll_cores": scores,
        })

    click(bar)
    click(bar)
    mark = now_on(session)

    for ch in "abcdefghijklmnopqrst":
        V.step(viewer, "type " + ch)
        time.sleep(0.33)

    time.sleep(2)
    V.step(viewer, "key Escape")
    keys = [k for m in meters_since(session, mark) for k in m[3]]

    lines = ["# Linux on %s, measured %s" % (address, datetime.datetime.now().strftime("%Y-%m-%d %H:%M")),
             "", "Epiphany in the Linux window, %dx%d; the Linux window's meter (`linux.lua`)." % (pw, ph), ""]

    for r in report:
        lines.append("## %s" % r["page"])
        lines.append("- loaded in **%s**, %.0f frames a second while it drew, longest gap %d ms"
                     % ("%.1f s" % r["load_s"] if r["load_s"] is not None else "?",
                        r["load_fps"], r["load_gap"]))
        lines.append("- scrolling: frames a second %s, longest gap **%d ms**"
                     % (" ".join(str(f) for f in r["scroll_fps"]) or "none", r["scroll_gap"]))
        lines.append("- while it loaded, %% of a core: %s"
                     % ", ".join("%s %d" % p for p in r["load_procs"][:6]))
        lines.append("- while it loaded, each core busy %%: %s" % " ".join(str(c) for c in r["load_cores"]))
        lines.append("- while it scrolled, %% of a core: %s"
                     % ", ".join("%s %d" % p for p in r["scroll_procs"][:6]))
        lines.append("- while it scrolled, each core busy %%: %s" % " ".join(str(c) for c in r["scroll_cores"]))
        lines.append("")

    if keys:
        lines.append("## typing")
        lines.append("- %d keys: median **%d ms** to the screen, fastest %d, slowest %d"
                     % (len(keys), statistics.median(keys), min(keys), max(keys)))
    else:
        lines.append("## typing\n- no key reached the screen")

    text = "\n".join(lines) + "\n"
    print(text)

    if out:
        os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
        with open(out, "w") as f:
            f.write(text)

    return 0


if __name__ == "__main__":
    sys.exit(main())
