#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""Processes and Monitor, the two windows about the machine (`roadmap.md`
6m and 6p), in one boot.

Both open at once, `wm sysmon,procs`, since the desktop holds the prompt
once it runs: Processes last, so it is in front for its checks, and Monitor
brought forward after them by a click on the part of it that shows.

**Processes**: every heading that sorts by something new pressed with
QEMU's tablet the way a person would. The window says where
its headings are and what its rows hold - `id:name:threads:privilege:file`
- once when it opens and again after each press, and this holds both to
what they should be:

- **the file**: Processes itself runs `/bin/procs.lua` and says so, and a
  server built into the image, the console, runs no file;
- **the threads**: one for a process with no workers, and threads of the
  kernel's own on its row, its idle threads among them;
- **the privilege**: EL0 for every process and EL1 for the kernel on
  AArch64, ring 3 and ring 0 on x86-64;
- **the order**: each press sorts by its column the way the column starts -
  a cost the most first, a word from the top - a second press on the same
  heading turns it round, a row with nothing in a column goes after every
  row with something whichever way, and ties go by id. The expected order
  is worked out here from the rows the window printed, not written down.

**Monitor**: each pace in its dots' menu chosen in turn.
What is held to the pace is the clock rather than the words: each time the
menu opens, Monitor says how many samples it has taken since the pace was
set and in how many seconds by the counter, and those have to agree with
the pace - a sample a second as it opens, then every half second, then
every two. It counted two of the kit's ticks to a column when the kit had
gone to one a second, which a check on the words could never have seen.

Usage: run_sysapps.py IMAGE
"""

import os
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import run_screenshot as R                                   # noqa: E402

# What each column sorts by, from a printed row, and which way it starts:
# the same table as `SORT_BY` and `DOWN_FIRST` in `procs.lua`, for the
# columns this presses.
KEY = {
    "id":        lambda r: r["id"],
    "name":      lambda r: r["name"].lower(),
    "file":      lambda r: r["file"],
    "threads":   lambda r: r["threads"],
    "privilege": lambda r: r["level"],
}
DOWN_FIRST = {"threads", "privilege"}


def parse(line):
    """The rows a `procs: ...:` line printed, in its order."""
    rows = []

    for item in (line or "").split("; "):
        parts = item.strip().split(":", 4)

        if len(parts) != 5:
            continue

        ident, name, threads, priv, path = parts
        rows.append({
            "id": int(ident), "name": name,
            "threads": None if threads == "-" else int(threads),
            "priv": priv,
            "level": 1 if priv in ("EL1", "EL2", "ring 0") else 0,
            "file": None if path == "-" else path,
        })

    return rows


def expected(rows, key, down):
    """`rows` as `procs.lua`'s `in_order` sorts them."""
    get = KEY[key]
    have = [r for r in rows if get(r) is not None]
    none = [r for r in rows if get(r) is None]

    have.sort(key=lambda r: r["id"])
    have.sort(key=get, reverse=down)       # stable: ties stay by id
    none.sort(key=lambda r: r["id"])
    return have + none


def main():
    image = sys.argv[1]
    x86 = "x86_64" in image
    guest = R.Guest(image, 120)
    failed = []
    checks = 0

    def check(ok, complaint):
        nonlocal checks
        checks += 1
        if not ok:
            failed.append(complaint)

    def said(text, since, seconds=30):
        """The rest of the line `text` begins, said after `since`."""
        deadline = time.monotonic() + seconds

        while time.monotonic() < deadline:
            at = guest.seen.find(text, since)

            if at >= 0 and "\n" in guest.seen[at + len(text):]:
                return guest.seen[at + len(text):].split("\n", 1)[0].strip()

            time.sleep(0.1)

        return None

    try:
        guest.wait_for("kosmos> ", "reached a prompt")
        start = len(guest.seen)
        mark = start
        guest.type("wm sysmon,procs")

        opened = said("procs: window at ", mark, 90)

        if opened is None:
            print("FAIL: Processes never opened its window.\n--- the guest said ---\n"
                  + guest.seen[mark:][-1500:])
            return 1

        ox, oy = (int(v) for v in opened.split(","))
        heads = dict((m.group(1), (int(m.group(2)), int(m.group(3))))
                     for m in re.finditer(r"(\w+) (\d+),(\d+)",
                                          said("procs: headings ", mark) or ""))
        rows = parse(said("procs: rows: ", mark))
        by_name = dict((r["name"], r) for r in rows)

        check(len(rows) > 5, "Processes listed %d rows" % len(rows))

        # The file each one runs.
        me = by_name.get("procs")
        check(me is not None and me["file"] == "/bin/procs.lua",
              "Processes does not say it runs /bin/procs.lua: %r" % me)
        console = by_name.get("console")
        check(console is not None and console["file"] is None,
              "the console, built into the image, says it runs a file: %r" % console)

        # Its threads, and the kernel's.
        check(me is not None and me["threads"] == 1,
              "Processes, which has no workers, says %r threads"
              % (me and me["threads"]))
        kernel = by_name.get("kernel")
        check(kernel is not None and kernel["threads"] is not None
              and kernel["threads"] >= 1,
              "the kernel's row has no threads of its own: %r" % kernel)

        # The privilege each runs at.
        user_word, kernel_word = ("ring 3", "ring 0") if x86 else ("EL0", "EL1")
        wrong = [r for r in rows if r["name"] != "kernel" and r["priv"] != user_word]
        check(not wrong, "a process not at %s: %r" % (user_word, wrong[:3]))
        check(kernel is not None and kernel["priv"] == kernel_word,
              "the kernel is not at %s: %r" % (kernel_word, kernel))

        width, height, _ = R.pixel_reader(guest.screendump())

        def press(key):
            x, y = heads[key]
            guest.mouse_to(*R._to_tablet(ox + x, oy + y, width, height))
            time.sleep(0.4)
            guest.mouse_button(True)
            time.sleep(0.3)
            guest.mouse_button(False)
            time.sleep(0.5)

        # Each heading, and one of them twice. The order is checked against
        # the rows the same line printed, so a process that came or went
        # between presses cannot make it wrong.
        for key, down in (("id", False), ("id", True), ("name", False),
                          ("file", False), ("threads", True),
                          ("privilege", True)):
            if key not in heads:
                check(False, "Processes did not say where its %s heading is" % key)
                continue

            mark = len(guest.seen)
            press(key)
            way = "descending" if down else "ascending"
            line = said("procs: sorted by %s, %s: " % (key, way), mark)

            if line is None:
                check(False, "pressing %s did not sort by it %s: %r"
                      % (key, way, said("procs: sorted by ", mark, 2)))
                continue

            got = parse(line)
            want = expected(got, key, down)
            check([r["id"] for r in got] == [r["id"] for r in want],
                  "sorted by %s, %s, the order is %s where it should be %s"
                  % (key, way, [r["id"] for r in got], [r["id"] for r in want]))

        # And where the rows with nothing in the column went.
        files = parse(said("procs: sorted by file, ascending: ", 0, 1))
        if files:
            gone = [r["file"] is None for r in files]
            check(gone == sorted(gone),
                  "sorted by file, a row with no file came before one with a file")

        # **Monitor**, under Processes, brought forward by a click on the
        # part of it Processes does not cover - found from the desktop's own
        # account of where each window is.
        opened = said("sysmon: window at ", start, 30)
        m = re.match(r"(\d+),(\d+); more at (\d+),(\d+)", opened or "")
        check(m is not None, "Monitor never said where its window and its dots are: %r"
              % opened)

        def rect(title):
            got = said("wm: window %s at " % title, start, 5)
            r = re.match(r"(\d+),(\d+) (\d+)x(\d+)", got or "")
            return tuple(int(v) for v in r.groups()) if r else None

        def click(x, y):
            guest.mouse_to(*R._to_tablet(x, y, width, height))
            time.sleep(0.4)
            guest.mouse_button(True)
            time.sleep(0.3)
            guest.mouse_button(False)
            time.sleep(0.5)

        mon, pro = rect("Monitor"), rect("Processes")
        showing = None

        if mon and pro:
            for x, y in ((mon[0] + 10, mon[1] + mon[3] - 10), (mon[0] + 10, mon[1] + 40),
                         (mon[0] + mon[2] - 10, mon[1] + mon[3] - 10)):
                if not (pro[0] <= x < pro[0] + pro[2] and pro[1] <= y < pro[1] + pro[3]):
                    showing = (x, y)
                    break

        check(showing is not None, "no part of Monitor shows beside Processes: %r, %r"
              % (mon, pro))

        if m and showing:
            mx0, my0, dx, dy = (int(v) for v in m.groups())
            click(*showing)

            def menu(pace):
                """The dots pressed: where the menu is, and how many samples
                in how long, held to `pace` seconds a sample."""
                mark_ = len(guest.seen)
                click(mx0 + dx, my0 + dy)
                got = said("sysmon: more menu at ", mark_)
                n = re.match(r"(\d+),(\d+), (\d+) wide, rows of (\d+); "
                             r"(\d+) samples in ([\d.]+) s", got or "")

                if not n:
                    check(False, "the dots did not open Monitor's menu: %r" % got)
                    return None

                x, y, w, rh, samples = (int(v) for v in n.groups()[:5])
                seconds = float(n.group(6))
                want = seconds / pace

                # The kit ticks at the pace or a little after it, never
                # before, and the first sample comes at once: so no more
                # than one over, and a pass slower than the pace under TCG
                # costs a little. Half the pace's count would be the old
                # fault, two ticks to a column.
                check(want * 0.75 - 1 <= samples <= want + 2,
                      "every %s s, Monitor took %d samples in %.1f s - %.1f is "
                      "the pace's" % (pace, samples, seconds, want))
                return x, y, rh

            # A second a sample, as it opens.
            time.sleep(8)
            at = menu(1)

            # Each pace chosen, and then the menu again after long enough to
            # count it.
            for row, pace, words, wait in ((0, 0.5, "update every 0.5 s, the last 30 seconds", 6),
                                           (2, 2, "update every 2 s, the last 2 minutes", 12),
                                           (1, 1, "update every 1 s, the last minute", 8)):
                if at is None:
                    break

                x, y, rh = at
                mark = len(guest.seen)
                click(x + 30, y + 2 + row * rh + rh // 2)
                got = said("sysmon: update every ", mark)
                check(got is not None and "update every " + got == words,
                      "choosing row %d of Monitor's menu did not say %r: %r"
                      % (row, words, got))
                time.sleep(wait)
                at = menu(pace)

            guest.sendkey("esc")
    finally:
        guest.close()

    if failed:
        print("FAIL: %d of %d checks on Processes and Monitor:" % (len(failed), checks))
        for f in failed:
            print("  " + f)
        return 1

    print("PASS: %d checks on Processes and Monitor (its file, its threads and its "
          "privilege in every row; six presses on five headings, each order as the "
          "column sorts; each of Monitor's three paces kept by the clock)" % checks)
    return 0


if __name__ == "__main__":
    sys.exit(main())
