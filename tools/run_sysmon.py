#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""System Monitor (`docs/sysmonitor.md`), as far as step S3 has built it:
the window, the Overview's tiles and the CPU page, in one boot.

`wm deskbar:--bar top,sysmon:--tell` - the Deskbar as a bar at the top,
which is where its meters are, and System Monitor saying what it shows.

- **The inventory check** (*How it is tested*): System Monitor prints the
  label and value of every row of `docs/sysmonitor.md`'s inventory on every
  page it can show, a row number each, and this holds them to the document
  itself - read here, not copied - for rows 1 to 18: each on the CPU page,
  and each whose "Goes to" names the Overview on the Overview as well. **A
  row missing from its page fails the suite.** It is the "lose nothing"
  contract made into a check that is never deleted; each later step widens
  `ROWS`.
- **QEMU's absences**: no GPU tile, in the driver's words; no battery; and
  "not measured on this machine" for every heat and speed row and each
  core's temperature - never a nought that looks like a reading.
- **The cost of a sample** said, by the counter and the collector, in the
  shape the M700 is to be read in (`--tell`; the numbers here are TCG's
  and are not held to anything).
- **Pages opened by a click**: the CPU tile, then the sidebar's Overview
  and CPU, each page's first line read. A tile pressed and slid off before
  the release opens nothing.
- **Monitor's checks, moved here with its window** from `run_sysapps.py`:
  each pace held to the counter, the processor named in a card with its
  chip and features, each core's bar shown and put away with the window
  growing and shrinking, and Control-W then Right moving the window.
- **The Deskbar's meters open the Overview** (inventory row 103).

**Every check has a control**, run in the same boot over what the guest
said with one thing broken - a row's line taken out, a heat row given a
nought, a GPU tile put in, half the samples a pace should have - and each
must fail. `--controls` runs them alone, on the host, over lines written
here.

Usage: run_sysmon.py IMAGE
       run_sysmon.py --controls
"""

import os
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

DOC = os.environ.get("KOSMOS_SYSMON_DOC") or os.path.join(ROOT, "docs", "sysmonitor.md")

# The rows of the inventory this step's pages answer for. S4 adds 19-41,
# S5 42-86, S6 96-99 and the Memory and Heat rows, S8 87-93.
ROWS = range(1, 19)

UNMEASURED = "not measured on this machine"

# What QEMU's processor does not give, by row: the speed now, the
# package's heat and power, why it is held, each core's speed.
QEMU_UNMEASURED = (6, 7, 8, 9, 12)

TOLD = re.compile(r"sysmon: row (\d+) (\w+): (.*?) = (.*)")


def inventory():
    """The document's rows: number -> where it goes, as written there."""
    rows = {}

    with open(DOC, encoding="utf-8") as f:
        for line in f:
            m = re.match(r"^\| (\d+) \| (.*) \| (.*) \| (.*) \|\s*$", line)

            if m:
                rows[int(m.group(1))] = m.group(4)

    return rows


def told(text):
    """What the window said, the latest of each: (row, page) -> value."""
    out = {}

    for m in TOLD.finditer(text):
        out[(int(m.group(1)), m.group(2))] = m.group(4).strip()

    return out


def missing(said, doc, rows=ROWS):
    """Each row of `rows` not told on the page the document sends it to."""
    out = []

    for n in rows:
        goes = doc.get(n)

        if goes is None:
            out.append("row %d is not in docs/sysmonitor.md's inventory" % n)
            continue

        if not said.get((n, "cpu")):
            out.append("row %d is not on the CPU page" % n)

        if "Overview" in goes and not said.get((n, "overview")):
            out.append("row %d goes to the Overview (%s) and is not there" % (n, goes))

    return out


def unmeasured_wrong(said, temperatures):
    """The QEMU rows that say anything but that they are not measured."""
    out = []

    for n in QEMU_UNMEASURED:
        for page in ("cpu", "overview"):
            v = said.get((n, page))

            if v is not None and v != UNMEASURED:
                out.append("row %d on %s says %r" % (n, page, v))

    if temperatures != UNMEASURED:
        out.append("each core's temperature says %r" % temperatures)

    return out


def tiles_wrong(tiles, gpu_why, battery):
    """What the Overview has that QEMU has not, or lacks that every machine has."""
    out = []
    names = [t.strip() for t in (tiles or "").split(",")]

    for want in ("cpu", "heat", "memory", "busiest"):
        if want not in names:
            out.append("no %s tile: %r" % (want, tiles))

    if "gpu" in names:
        out.append("a GPU tile under QEMU: %r" % tiles)

    if gpu_why != "no GPU this driver knows":
        out.append("the GPU's absence not said in the driver's words: %r" % gpu_why)

    if battery != "none":
        out.append("a battery under QEMU: %r" % battery)

    return out


COST = re.compile(r"asking ([\d.]+) ms, differences ([\d.]+) ms, painting ([\d.]+) ms; "
                  r"(\d+) messages, (\d+) calls; allocated (.+?) sampling, (.+?) painting")


def cost_wrong(line):
    m = COST.search(line or "")

    if not m:
        return ["the cost of a sample was not said: %r" % line]

    if int(m.group(5)) < 3 or int(m.group(4)) < 2:
        return ["a sample asked fewer than the processors, the machine and its processes: %r"
                % line]

    return []


def pace_wrong(samples, seconds, pace):
    """The kit ticks at the pace or a little after it, never before, and the
    first sample comes at once: so no more than one over, and a pass slower
    than the pace under TCG costs a little. Half the pace's count would be
    the old fault, two ticks to a column."""
    want = seconds / pace

    if want * 0.75 - 1 <= samples <= want + 2:
        return []

    return ["every %s s, System Monitor took %d samples in %.1f s - %.1f is the pace's"
            % (pace, samples, seconds, want)]


def controls(text, said, doc, temperatures, tiles, gpu_why, battery, cost, pace_seen):
    """Each check, over what was said with one thing broken: each must fail.
    Returns the checks that did not."""
    bitten = []

    # A row's line taken out of the CPU page.
    if not missing({k: v for k, v in said.items() if k != (7, "cpu")}, doc):
        bitten.append("the inventory did not miss row 7 taken off the CPU page")

    # The Overview's small graphs taken off the Overview.
    if not missing({k: v for k, v in said.items() if k != (13, "overview")}, doc):
        bitten.append("the inventory did not miss row 13 taken off the Overview")

    # A row the document has, asked for though no page tells it.
    if not missing(said, doc, rows=[19]):
        bitten.append("the inventory passed row 19, which no page tells yet")

    # A nought where a reading should be absent.
    broken = dict(said)
    broken[(7, "cpu")] = "0 °C"
    if not unmeasured_wrong(broken, temperatures):
        bitten.append("a temperature of nought under QEMU was not caught")

    if not unmeasured_wrong(said, "1:0 °C"):
        bitten.append("a core's temperature of nought under QEMU was not caught")

    if not tiles_wrong((tiles or "") + ", gpu", gpu_why, battery):
        bitten.append("a GPU tile under QEMU was not caught")

    if not tiles_wrong(tiles, gpu_why, "50% charging"):
        bitten.append("a battery under QEMU was not caught")

    if not cost_wrong((cost or "").replace("messages", "requests")):
        bitten.append("a cost line not in its shape was not caught")

    for samples, seconds, pace in pace_seen:
        if not pace_wrong(samples // 2, seconds, pace):
            bitten.append("half the samples of a %s s pace were not caught" % pace)

    return bitten


def selftest():
    """The controls alone, on the host, over lines in the shape the window
    prints - the document's own rows, told as the CPU and Overview pages
    tell them."""
    doc = inventory()
    lines = []

    for n in ROWS:
        value = UNMEASURED if n in QEMU_UNMEASURED else "something"
        lines.append("sysmon: row %d cpu: a row = %s" % (n, value))

        if "Overview" in doc.get(n, ""):
            lines.append("sysmon: row %d overview: a row = %s" % (n, value))

    text = "\n".join(lines) + "\n"
    said = told(text)
    tiles = "cpu, heat, memory, network, disks, busiest"
    cost = ("sysmon: cost of a sample: asking 1.20 ms, differences 0.10 ms, painting 3.40 ms; "
            "5 messages, 3 calls; allocated 6.1 KB sampling, 9.0 KB painting")
    failed = (missing(said, doc) + unmeasured_wrong(said, UNMEASURED)
              + tiles_wrong(tiles, "no GPU this driver knows", "none") + cost_wrong(cost)
              + pace_wrong(10, 10.2, 1))
    bitten = controls(text, said, doc, UNMEASURED, tiles, "no GPU this driver knows", "none",
                      cost, [(10, 10.2, 1), (20, 10.1, 0.5)])

    for f in failed:
        print("  the good lines failed: " + f)
    for b in bitten:
        print("  a control did not bite: " + b)

    if failed or bitten:
        print("FAIL: the checks themselves")
        return 1

    print("PASS: every check passes the good lines and fails its control "
          "(rows %d-%d of %d in the document)" % (ROWS[0], ROWS[-1], len(doc)))
    return 0


def main():
    if sys.argv[1:] == ["--controls"]:
        return selftest()

    import run_screenshot as R                               # noqa: E402

    image = sys.argv[1]
    doc = inventory()
    guest = R.Guest(image, 120)
    failed = []
    checks = 0
    pace_seen = []

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
        guest.type("wm deskbar:--bar top,sysmon:--tell")

        opened = said("sysmon: window at ", start, 90)
        m = re.match(r"(\d+),(\d+); more at (\d+),(\d+)", opened or "")

        if m is None:
            print("FAIL: System Monitor never said where its window is.\n--- the guest said ---\n"
                  + guest.seen[start:][-1500:])
            return 1

        ox, oy, dx, dy = (int(v) for v in m.groups())
        width, height, _ = R.pixel_reader(guest.screendump())

        def click(x, y, slide=None):
            guest.mouse_to(*R._to_tablet(x, y, width, height))
            time.sleep(0.4)
            guest.mouse_button(True)
            time.sleep(0.3)
            if slide:
                guest.mouse_to(*R._to_tablet(x + slide[0], y + slide[1], width, height))
                time.sleep(0.3)
            guest.mouse_button(False)
            time.sleep(0.5)

        # ---- The Overview, as it opens ----
        first = said("sysmon: page overview shown: tiles ", start, 10)
        gpu_why = said("sysmon: no GPU tile: ", start, 5)
        check(first is not None, "System Monitor did not open on its Overview")

        # The inventory, once the window has three samples to tell.
        check(said("sysmon: row 18 cpu: ", start, 60) is not None,
              "System Monitor did not tell its inventory with --tell")
        time.sleep(1)
        text = guest.seen[start:]
        rows = told(text)
        temperatures = said("sysmon: core temperatures ", start, 5)
        cost = said("sysmon: cost of a sample: ", start, 5)
        battery = rows.get((102, "overview"))

        def holds(complaints, what):
            check(not complaints, what + ": " + "; ".join(complaints))

        holds(missing(rows, doc), "the inventory, rows %d-%d" % (ROWS[0], ROWS[-1]))
        holds(unmeasured_wrong(rows, temperatures), "under QEMU")
        holds(tiles_wrong(first, gpu_why, battery), "the Overview's tiles")
        holds(cost_wrong(cost), "the cost of a sample")

        # ---- Pages by a click ----
        tiles = dict((t.group(1), tuple(int(v) for v in t.groups()[1:]))
                     for t in re.finditer(r"(\w+) at (\d+),(\d+) (\d+)x(\d+)",
                                          said("sysmon: tiles ", start, 5) or ""))
        side = dict((s.group(1), (int(s.group(2)), int(s.group(3))))
                    for s in re.finditer(r"sysmon: sidebar (\S+) at (\d+),(\d+)", guest.seen[start:]))
        check("cpu" in tiles and "overview" in side and "cpu" in side,
              "System Monitor did not say where its tiles and its sidebar's rows are: %r, %r"
              % (tiles, side))

        if "cpu" in tiles:
            x, y, w, h = tiles["cpu"]

            # The control: pressed on the tile and let go below the window.
            mark = len(guest.seen)
            click(ox + x + w // 2, oy + y + 20, slide=(0, 2000 - (oy + y)))
            time.sleep(1)
            check("sysmon: page cpu shown" not in guest.seen[mark:],
                  "the CPU tile pressed and slid off before the release opened its page")

            mark = len(guest.seen)
            click(ox + x + w // 2, oy + y + 20)
            got = said("sysmon: page cpu shown: ", mark, 10)
            check(got is not None and re.fullmatch(r"\d+ processors? · \d+% busy", got),
                  "a click on the CPU tile did not show the CPU page: %r" % got)

        for page, want in (("overview", r"tiles cpu, .*busiest"),
                           ("cpu", r"\d+ processors? · \d+% busy")):
            if page not in side:
                continue

            mark = len(guest.seen)
            click(ox + side[page][0], oy + side[page][1])
            got = said("sysmon: page %s shown: " % page, mark, 10)
            check(got is not None and re.fullmatch(want, got),
                  "the sidebar's %s did not show its page: %r" % (page, got))

        # ---- Monitor's checks, on the CPU page ----
        def menu(pace):
            """The dots pressed: where the menu is, and how many samples in
            how long, held to `pace` seconds a sample."""
            mark_ = len(guest.seen)
            click(ox + dx, oy + dy)
            got = said("sysmon: more menu at ", mark_)
            n = re.match(r"(\d+),(\d+), (\d+) wide, rows of (\d+); "
                         r"(\d+) samples in ([\d.]+) s", got or "")

            if not n:
                check(False, "the dots did not open System Monitor's menu: %r" % got)
                return None

            x, y, _, rh, samples = (int(v) for v in n.groups()[:5])
            seconds = float(n.group(6))
            pace_seen.append((samples, seconds, pace))

            check(not pace_wrong(samples, seconds, pace), "; ".join(pace_wrong(samples, seconds, pace)))
            return x, y, rh

        # A second a sample, as it opens; then each pace chosen, and the
        # menu again after long enough to count it.
        time.sleep(6)
        at = menu(1)

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
                  "choosing row %d of the menu did not say %r: %r" % (row, words, got))
            time.sleep(wait)
            at = menu(pace)

        # The processor said once and whole, its chip 64 square with the
        # maker's word on the lid, and its features without what QEMU has not.
        named = said("sysmon: processor ", start, 5)
        check(named is not None and named.count("|") == 2 and "processor" in named,
              "System Monitor did not name its processor in a card: %r" % named)

        chip = said("sysmon: chip ", start, 5)
        parts = [p.strip() for p in named.split("|")] if named else []
        maker = parts[0] if parts else ""
        model = parts[1] if len(parts) > 1 else ""
        want_word = (maker.split() or model.split() or [""])[0]
        check(chip is not None and chip.startswith("%s |" % want_word)
              and chip.endswith(", 64 square"),
              "the chip is not 64 square with %r on its lid: %r" % (want_word, chip))

        feats = said("sysmon: features", start, 5)
        check(feats is not None and "VT-x on" not in feats,
              "System Monitor did not say its processor's features, or claimed what QEMU has not: %r"
              % feats)

        # Each core's bar, off as it opens, the page growing to hold them and
        # back when they are put away - below the three paces and a
        # separator a third of a row. The window is the taller of the page
        # and the sidebar, which on QEMU's few cores is the sidebar, so it is
        # the page that is held to growing and the window to that rule.
        sizes, fits = [], []

        for want in ("bars shown", "bars hidden"):
            if at is None:
                break

            x, y, rh = at
            mark = len(guest.seen)
            click(x + 30, y + 2 + 3 * rh + rh // 3 + rh // 2)
            got = said("sysmon: " + want + ", ", mark)
            size = re.match(r"(\d+)x(\d+), its page (\d+), the sidebar (\d+)", got or "")
            check(size is not None, "choosing each core's bar did not say %r: %r" % (want, got))
            sizes.append(int(size.group(3)) if size else 0)
            fits.append(int(size.group(2)) - max(int(size.group(3)), int(size.group(4)))
                        if size else None)
            time.sleep(1)
            at = menu(1) if want == "bars shown" else None

        check(len(sizes) == 2 and sizes[0] > sizes[1] > 0,
              "the bars did not grow the page and put away shrink it: %r" % sizes)
        # The same header above the taller of the two, both times - and a
        # header there at all, which a resize to the page alone left out.
        check(len(fits) == 2 and fits[0] is not None and fits[0] == fits[1] and fits[0] > 0,
              "the window was not its header above the taller of its page and "
              "its sidebar: %r" % fits)

        guest.sendkey("esc")
        time.sleep(0.5)

        # Control-W then an arrow moves the window in front - the window
        # manager's own keys (`roadmap.md` 6n, step 0).
        placed = said("wm: window System Monitor at ", start, 5)
        r = re.match(r"(\d+),(\d+)", placed or "")
        mark = len(guest.seen)
        guest.proc.stdin.write(b"\x17\x1b[C")
        guest.proc.stdin.flush()
        got = said("wm: moved ", mark, 10)
        moved = re.match(r"System Monitor to (\d+),(\d+)", got or "")
        check(moved is not None and r is not None
              and (int(moved.group(1)), int(moved.group(2))) == (int(r.group(1)) + 16, int(r.group(2))),
              "Control-W then Right did not move System Monitor 16 to the right of %r: %r"
              % (placed, got))

        # ---- The Deskbar's meters open the Overview (row 103) ----
        meters = re.findall(r"deskbar: the meters at (\d+)", guest.seen[start:])
        bar = said("wm: window Deskbar at ", start, 5)
        b = re.match(r"(\d+),(\d+) (\d+)x(\d+)", bar or "")
        check(meters and b, "the Deskbar did not say where its meters are: %r, %r" % (meters, bar))

        if meters and b:
            bx, by, _, bh = (int(v) for v in b.groups())
            mark = len(guest.seen)
            click(bx + int(meters[-1]) + 13, by + bh // 2)
            asked = said("deskbar: System Monitor asked for, the Overview", mark, 10)
            opened = said("sysmon: page overview shown: tiles ", mark, 60)
            check(asked is not None and opened is not None,
                  "a click on the Deskbar's meters did not open System Monitor's Overview: %r, %r"
                  % (asked, opened))

        # The controls, over what this boot said.
        for b_ in controls(text, rows, doc, temperatures, first, gpu_why, battery, cost, pace_seen):
            check(False, "control: " + b_)

        time.sleep(1)
        check("sysmon.lua:" not in guest.seen[start:] and "System Monitor stopped" not in guest.seen[start:],
              "System Monitor said an error: %r" % guest.seen[start:][-800:])
    finally:
        guest.close()

    if failed:
        print("FAIL: %d of %d checks on System Monitor:" % (len(failed), checks))
        for f in failed:
            print("  " + f)
        return 1

    print("PASS: %d checks on System Monitor (rows %d-%d of the inventory on their pages, "
          "QEMU's absences said, a sample's cost said, the CPU tile and the sidebar opening "
          "their pages, three paces kept by the clock, the processor's card, each core's bar "
          "shown and put away, Control-W moving it, and the Deskbar's meters opening the "
          "Overview; every check's control bit)" % (checks, ROWS[0], ROWS[-1]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
