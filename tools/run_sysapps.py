#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""Processes, the window about what is running (`roadmap.md` 6m), in one
boot.

`wm procs`. Every heading that sorts by something new pressed with QEMU's
tablet the way a person would. The window says where its headings are and
what its rows hold - `id:name:threads:privilege:file` - once when it opens
and again after each press, and this holds both to what they should be:

- **the file**: Processes itself runs `/Kosmos/Apps/procs.lua` and says so, and a
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

**Monitor's half moved to `run_sysmon.py`** with its window, which is
System Monitor's CPU page since `docs/sysmonitor.md` S3: its paces held to
the counter, its processor's card, its bars and Control-W. Processes' half
goes there in S4, when Processes becomes a page (`sysmonitor.md`, *How it
is tested*), and this suite with it in S9.

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
    # **The processor choosing its own speed**, asked for on the command
    # line (`roadmap.md`, "The processor's speed", step 2): the kernel says
    # it heard, and a machine whose processor has no such control - QEMU's -
    # comes up as it always did, every core saying its speed is the
    # firmware's.
    R.extra_args(image, ["-fw_cfg", "name=opt/kosmos/speed,string=battery"])

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

        # **The processor's heat and power** (`roadmap.md`): QEMU's processor
        # is not an Intel the kernel knows to keep them, so every core's tick
        # has asked and been told no - without a fault - and `clocks` says
        # so rather than showing a temperature nobody read.
        heard = len(guest.seen)
        guest.type("clocks")
        check(said("says nothing of its heat or its power to this kernel", heard, 30) is not None,
              "clocks did not say QEMU's processor keeps no heat or power")
        # And the prompt back after it, before the next command is typed.
        deadline = time.monotonic() + 30
        while guest.seen.find("kosmos> ", heard) < 0 and time.monotonic() < deadline:
            time.sleep(0.1)

        start = len(guest.seen)
        mark = start
        guest.type("wm procs")

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
        check(me is not None and me["file"] == "/Kosmos/Apps/procs.lua",
              "Processes does not say it runs /Kosmos/Apps/procs.lua: %r" % me)
        console = by_name.get("console")
        check(console is not None and console["file"] is None,
              "the console, built into the image, says it runs a file: %r" % console)

        # Its threads, and the kernel's.
        check(me is not None and me["threads"] == 1,
              "Processes, which has no workers, says %r threads"
              % (me and me["threads"]))
        kernel = by_name.get("nebula")
        check(kernel is not None and kernel["threads"] is not None
              and kernel["threads"] >= 1,
              "the kernel's row has no threads of its own: %r" % kernel)

        # **What each is and how it draws** (Diego, on the M700, 10
        # October): the kernel by its name, Nebula, and its kind; the
        # window server composing the screen; Processes drawn by the kit;
        # a server, with no window, blank.
        drawn = {}
        for item in (said("procs: drawn: ", mark) or "").split("; "):
            parts = item.strip().split(":")
            if len(parts) >= 3:
                drawn[parts[0]] = (parts[1], parts[2], parts[3] if len(parts) > 3 else "")
        # Memory as Tracker writes a size: a megabyte and more in MB (Diego,
        # 14 November 2026), and the window manager holds more than one.
        sizes = [v[2] for v in drawn.values() if v[2] != "-"]
        check(sizes and all(re.fullmatch(r"\d+(\.\d)? (B|KB|MB|GB)", m) for m in sizes)
              and drawn.get("wm", ("", "", ""))[2].endswith(" MB"),
              "memory is not written as a size, the window manager's in MB: wm %r, %r"
              % (drawn.get("wm"), sizes[:6]))
        check(drawn.get("nebula", ("", ""))[0] == "kernel",
              "the kernel's row is not Nebula, of kind kernel: %r" % (drawn.get("nebula"),))
        check(drawn.get("windowserver", ("", ""))[1] == "screen"
              and drawn.get("procs", ("", ""))[1] == "kit"
              and drawn.get("binfs", ("", "x"))[1] == "-",
              "draws does not say screen, kit and nothing: windowserver %r, procs %r, binfs %r"
              % (drawn.get("windowserver"), drawn.get("procs"), drawn.get("binfs")))

        # The privilege each runs at.
        user_word, kernel_word = ("ring 3", "ring 0") if x86 else ("EL0", "EL1")
        wrong = [r for r in rows if r["name"] != "nebula" and r["priv"] != user_word]
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

        # A heading pressed and slid off before the release sorts nothing: a
        # click is a press and a release (Diego, 9 October, "in all kosmos").
        if "name" in heads:
            x, y = heads["name"]
            mark = len(guest.seen)
            guest.mouse_to(*R._to_tablet(ox + x, oy + y, width, height))
            time.sleep(0.4)
            guest.mouse_button(True)
            time.sleep(0.3)
            guest.mouse_to(*R._to_tablet(ox + x, oy + y + 200, width, height))
            time.sleep(0.3)
            guest.mouse_button(False)
            time.sleep(1.0)
            check(said("procs: sorted by ", mark, 1) is None,
                  "a heading pressed and slid off before the release sorted the rows")

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

        # **The kit's columns** (`ui.columns`): name's edge dragged 40 wider
        # and kept, and the headings' menu by a right click.
        edges = dict((k, int(v)) for k, v in re.findall(r"(\w+)@(\d+)",
                                                       said("procs: edges ", 0, 2) or ""))
        if "name" in edges and "name" in heads:
            hy = heads["name"][1]
            mark = len(guest.seen)
            guest.mouse_to(*R._to_tablet(ox + edges["name"], oy + hy, width, height))
            time.sleep(0.3)
            guest.mouse_button(True)
            time.sleep(0.2)
            for step in range(1, 5):
                guest.mouse_to(*R._to_tablet(ox + edges["name"] + 10 * step, oy + hy, width, height))
                time.sleep(0.1)
            guest.mouse_button(False)
            dragged = said("procs: column name now ", mark, 5)
            kept = said("procs: columns ", mark, 5)
            m = re.match(r"(\d+)", dragged or "")
            check(m is not None and kept is not None and ("name %s" % m.group(1)) in kept,
                  "Processes' name column dragged wider was not kept: %r, %r" % (dragged, kept))

            mark = len(guest.seen)
            guest.mouse_to(*R._to_tablet(ox + heads["kind"][0], oy + hy, width, height))
            time.sleep(0.3)
            guest.mouse_button(True, "right")
            time.sleep(0.1)
            guest.mouse_button(False, "right")
            check(said("procs: the headings' menu", mark, 5) is not None,
                  "a right click on Processes' headings did not open their menu")
            guest.sendkey("esc")
            time.sleep(0.5)
        else:
            check(False, "Processes did not say where its headings' edges are")

        # And where the rows with nothing in the column went.
        files = parse(said("procs: sorted by file, ascending: ", 0, 1))
        if files:
            gone = [r["file"] is None for r in files]
            check(gone == sorted(gone),
                  "sorted by file, a row with no file came before one with a file")

        check("the processor chooses its own speed, leaning 192 of 255" in guest.seen,
              "opt/kosmos/speed=battery was not heard at boot as a leaning of 192")

        # **Nebula's own memory** (10 October: "why does the kernel does not
        # show any memory consumption in processes?"): its row holds what
        # the kernel counts for itself - more than its image, less than the
        # machine.
        held = said("procs: nebula holds ", 0, 10)
        n = re.match(r"(\d+) KB of (\d+) KB", held or "")
        check(n is not None and 1024 < int(n.group(1)) < int(n.group(2)),
              "Processes' Nebula row did not hold the kernel's own memory: %r" % held)
    finally:
        guest.close()

    if failed:
        print("FAIL: %d of %d checks on Processes:" % (len(failed), checks))
        for f in failed:
            print("  " + f)
        return 1

    print("PASS: %d checks on Processes (its columns dragged and their menu; its file, its threads and its "
          "privilege in every row; six presses on five headings, each order as the "
          "column sorts; Nebula's own memory)" % checks)
    return 0


if __name__ == "__main__":
    sys.exit(main())
