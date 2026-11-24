#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""System Monitor (`docs/sysmonitor.md`), as far as step S5 has built it:
the window, the Overview's tiles, the CPU, Processes, GPU, Disks and
Network pages, in two boots that run side by side (*How it is tested*: the
suite in two halves, to keep within the budget).

`wm deskbar:--bar top,sysmon:--tell,spin:600` - the Deskbar as a bar at
the top, which is where its meters are, System Monitor saying what it
shows, and a process for its End to end.

- **The inventory check** (*How it is tested*): System Monitor prints the
  label and value of every row of `docs/sysmonitor.md`'s inventory on every
  page it can show, a row number each, and this holds them to the document
  itself - read here, not copied - for rows 1 to 86: each on its own page
  (1-18 the CPU page, 19-41 Processes, 42-60 the GPU, 61-74 Disks, 75-86
  Network), and each whose "Goes to" names the Overview on the Overview as
  well - where this machine has the part: under QEMU there is no GPU tile,
  so the GPU's rows are not asked of the Overview there. **A row missing
  from its page fails the suite.** It is the "lose nothing"
  contract made into a check that is never deleted; each later step widens
  `ROWS`.
- **QEMU's absences**: no GPU tile and no GPU row in the sidebar, in the
  driver's words; the GPU page's "No GPU this driver knows" and "Under
  QEMU there is no Intel GPU to watch." (row 59, GPU Monitor's words,
  moved here from `run_gpumon.py`) and its power and temperature not
  measured; no battery; and "not measured on this machine" for every heat
  and speed row and each core's temperature - never a nought that looks
  like a reading.
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
- **Processes' checks, moved here with its table** from `run_sysapps.py`
  (S4): the page opened from the sidebar, each heading that sorts by
  something new pressed - and one twice - and the order held to what this
  works out from the rows the page printed; a heading pressed and slid off
  sorting nothing; a row with no file after every row with one; Nebula's
  row at ring 0. `parse`, `expected` and the presses are here, and
  `run_sysapps.py` reads them from here for Process Viewer until S9.
- **End on `spin`**: its row chosen, End pressed, and the process gone
  from the list after it.
- **The strip**: the process chosen has its minute at once, from before it
  was chosen - its share and memory over more than the moment of the
  click.
- **The Deskbar's meters open the Overview** (inventory row 103).

**The second half, `--io`** (S5), boots with a FAT stick and a network
card, a program named `reader` reading the stick's file and one named
`pull` asking this Mac for a megabyte at a time, and opens System Monitor
on its GPU page (`wm sysmon:--tell gpu`) - Disk Monitor's and Network
Monitor's checks, moved here from `run_diskmon.py` and `run_netmon.py`:

- the GPU page opened from outside, saying there is no GPU in the
  driver's words (GPU Monitor's, from `run_gpumon.py`);
- the inventory's rows 42-86 on their pages again, with the stick and the
  card there;
- the stick's row in the sidebar opening the Disks page with it chosen, a
  rate of reading on it, `reader` the busiest program by its name, and
  its minute mirrored - `io_in` blue above the centre line, none below;
- the sidebar's Network opening its page, a rate in from `pull`'s
  megabytes, `pull`'s connection the busiest by its name, its minute
  mirrored the same way;
- "Rates in bits a second" from the dots, heard;
- **the strip** gaining what S5 adds: `reader` chosen on the Processes
  page has the stick among its drives, and `pull` its connection to this
  Mac.

**Every check has a control**, run in the same boot over what the guest
said with one thing broken - a row's line taken out, a heat row given a
nought, a GPU tile put in, half the samples a pace should have, two rows
of a sorted list swapped, the line saying the ended process went taken
out, a strip with no minute, Nebula at ring 3 - and each must fail. `--controls` runs them alone, on the host, over lines written
here.

Usage: run_sysmon.py IMAGE
       run_sysmon.py IMAGE --io
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

# The rows of the inventory this step's pages answer for, each on the page
# that is its home. S6 adds 96-99 and the Memory and Heat rows, S8 87-93.
ROWS = range(1, 87)
HOMES = ((range(1, 19), "cpu"), (range(19, 42), "processes"), (range(42, 61), "gpu"),
         (range(61, 75), "disks"), (range(75, 87), "network"))

# The part a row's Overview tile is - asked of the Overview only where the
# machine has that part (*Nothing that is absent gets a tile*). Row 85 is
# the Overview's header, which every machine has.
PARTS = ((range(42, 61), "gpu"), (range(61, 75), "disks"), (range(75, 85), "network"),
         (range(86, 87), "network"))


def part(n):
    for rows, name in PARTS:
        if n in rows:
            return name

    return None


def home(n):
    """The page row `n` is told on, besides the Overview."""
    for rows, page in HOMES:
        if n in rows:
            return page

    return None

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


def missing(said, doc, rows=ROWS, parts=None):
    """Each row of `rows` not told on the page the document sends it to.
    `parts` is the set of parts this machine has - the sidebar's rows - and
    None is every part."""
    out = []

    for n in rows:
        goes = doc.get(n)

        if goes is None:
            out.append("row %d is not in docs/sysmonitor.md's inventory" % n)
            continue

        if not said.get((n, home(n))):
            out.append("row %d is not on the %s page" % (n, home(n)))

        present = parts is None or part(n) is None or part(n) in parts

        if "Overview" in goes and present and not said.get((n, "overview")):
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


NO_GPU = "No GPU this driver knows"
NO_GPU_HERE = "Under QEMU there is no Intel GPU to watch."


def gpu_absent_wrong(said, side, gpu_temperature):
    """The GPU page under QEMU: the card's and the page's sentences (row 59),
    its power and temperature not measured, and no GPU row in the sidebar."""
    out = []
    v = said.get((59, "gpu")) or ""

    if NO_GPU not in v or "no GPU this driver knows" not in v or NO_GPU_HERE not in v:
        out.append("the GPU page did not say there is no GPU in GPU Monitor's words: %r" % v)

    if said.get((54, "gpu")) != UNMEASURED:
        out.append("the GPU page's power under QEMU says %r" % said.get((54, "gpu")))

    if gpu_temperature != UNMEASURED:
        out.append("the GPU page's temperature under QEMU says %r" % gpu_temperature)

    if "gpu" in side:
        out.append("a GPU row in the sidebar under QEMU")

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


# ---- Processes' headings, moved here from `run_sysapps.py` (S4) ----

# What each column sorts by, from a printed row, and which way it starts:
# the same table as `SORT_BY` and `DOWN_FIRST` in `sysmon/processes.lua`
# (and `procs.lua`'s, until S9), for the columns this presses.
KEY = {
    "id":        lambda r: r["id"],
    "name":      lambda r: r["name"].lower(),
    "file":      lambda r: r["file"],
    "threads":   lambda r: r["threads"],
    "privilege": lambda r: r["level"],
}
DOWN_FIRST = {"threads", "privilege"}

# Each heading that sorts by something new, and one of them twice.
SORTS = (("id", False), ("id", True), ("name", False), ("file", False),
         ("threads", True), ("privilege", True))


def parse(line):
    """The rows a `... rows:` or `... sorted by ...:` line printed, in its
    order: `id:name:threads:privilege:file`, `; ` between two."""
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
    """`rows` as the table's `in_order` sorts them."""
    get = KEY[key]
    have = [r for r in rows if get(r) is not None]
    none = [r for r in rows if get(r) is None]

    have.sort(key=lambda r: r["id"])
    have.sort(key=get, reverse=down)       # stable: ties stay by id
    none.sort(key=lambda r: r["id"])
    return have + none


def order_wrong(got, key, down):
    """What is wrong with `got` as the order sorting by `key` gives."""
    want = expected(got, key, down)

    if [r["id"] for r in got] == [r["id"] for r in want]:
        return []

    return ["sorted by %s, %s, the order is %s where it should be %s"
            % (key, "descending" if down else "ascending",
               [r["id"] for r in got], [r["id"] for r in want])]


def swapped(rows, key):
    """`rows` with the first two that differ in `key` swapped - a sorted
    list made wrong, for a control - or None when all agree."""
    get = KEY[key]

    for i in range(len(rows) - 1):
        if get(rows[i]) != get(rows[i + 1]):
            out = list(rows)
            out[i], out[i + 1] = out[i + 1], out[i]
            return out

    return None


def sort_checks(heads, press, slide, said, now, check, prefix):
    """Every heading in `SORTS` pressed, a table's order held to `expected`,
    and a heading slid off before the release sorting nothing. `prefix` is
    what the table's lines begin with ("procs: ", "sysmon: processes ").
    Returns `(key, down, rows)` for each order it read, for the controls."""
    seen = []

    if "name" in heads:
        mark = now()
        slide("name")
        check(said(prefix + "sorted by ", mark, 1) is None,
              "a heading pressed and slid off before the release sorted the rows")

    for key, down in SORTS:
        if key not in heads:
            check(False, "the table did not say where its %s heading is" % key)
            continue

        mark = now()
        press(key)
        way = "descending" if down else "ascending"
        line = said("%ssorted by %s, %s: " % (prefix, key, way), mark)

        if line is None:
            check(False, "pressing %s did not sort by it %s: %r"
                  % (key, way, said(prefix + "sorted by ", mark, 2)))
            continue

        got = parse(line)
        seen.append((key, down, got))
        problems = order_wrong(got, key, down)
        check(not problems, "; ".join(problems))

        # And where the rows with nothing in the column went.
        if key == "file":
            gone = [r["file"] is None for r in got]
            check(gone == sorted(gone),
                  "sorted by file, a row with no file came before one with a file")

    return seen


def strip_wrong(line, ident, name):
    """The strip said on choosing a process: that process, with more of its
    minute than the moment it was chosen in."""
    m = re.match(r"(\d+) (\S+): share (\d+) columns, now \d+%; memory (\d+) columns", line or "")

    if not m:
        return ["the strip was not said on choosing %s: %r" % (name, line)]

    if int(m.group(1)) != ident or m.group(2) != name:
        return ["the strip follows %s %s, not the %d %s chosen" % (m.group(1), m.group(2), ident, name)]

    if int(m.group(3)) < 3 or int(m.group(4)) < 3:
        return ["the strip had %s and %s columns of %s's minute as it was chosen - not the minute "
                "before" % (m.group(3), m.group(4), name)]

    return []


# ---- Disk Monitor's, Network Monitor's and the strip's (S5, `--io`) ----

# The theme's `io_in`, in the dark palette and the light: whichever look
# the desktop is in, it is one of the two (`run_netmon.py`'s, moved here).
IO_IN = ((0x4f, 0xb3, 0xff), (0x1f, 0x7f, 0xd0))

DISK_LINE = re.compile(r"sysmon: disks (\S+) read (\d+) B/s, wrote (\d+) B/s; busiest (\S+) (\d+) (\d+)")
NET_LINE = re.compile(r"sysmon: network in (\d+) B/s, out (\d+) B/s; (\d+) open; busiest (\S+) (\d+) (\d+)")


def disk_best(text, key, who):
    """The Disks page's lines for drive `key` whose busiest program is
    `who`, the one where `who` read the most - by its own rate, since the
    first reading after a drive is chosen has nothing of it to subtract
    from - as `(drive's rate, who's rate)`, or None."""
    best = None

    for m in DISK_LINE.finditer(text):
        if m.group(1) == key and m.group(4) == who:
            got = (int(m.group(2)), int(m.group(5)))
            if best is None or got[1] > best[1]:
                best = got

    return best


def disk_wrong(best, key, who, least=2000):
    if best is None:
        return ["the Disks page never saw %s read by %s" % (key, who)]

    out = []

    if best[0] <= least:
        out.append("the Disks page's rate of reading %s stayed at %d B/s" % (key, best[0]))
    if best[1] <= least:
        out.append("%s's row did not carry the rate: %d B/s" % (who, best[1]))

    return out


def net_best(text, who):
    """The Network page's lines whose busiest connection is `who`'s: the
    best rate in, and the best of `who`'s own - each at its best, since a
    sample with the most bytes can fall where the connection is between two
    requests (`run_netmon.py`, 12 and 13 October)."""
    best_in, best_conn = None, None

    for m in NET_LINE.finditer(text):
        if m.group(4) != who:
            continue

        if best_in is None or int(m.group(1)) > best_in:
            best_in = int(m.group(1))
        if best_conn is None or int(m.group(5)) > best_conn:
            best_conn = int(m.group(5))

    return best_in, best_conn


def net_wrong(best, who, least_in=200000, least_conn=100000):
    best_in, best_conn = best
    out = []

    if best_in is None or best_in <= least_in:
        out.append("the Network page never saw a rate in from %s's megabytes: %r" % (who, best_in))
    if best_conn is None or best_conn <= least_conn:
        out.append("%s's connection did not carry the rate: %r" % (who, best_conn))

    return out


def graph_wrong(above, below, what):
    """A mirrored minute of reading or in: `io_in` blue above its centre
    line (`above`, where it was found, or None) and none below."""
    out = []

    if above is None:
        out.append("no io_in blue above %s's centre line" % what)
    if below is not None:
        out.append("io_in blue below %s's centre line, where writing and out belong: %r" % (what, below))

    return out


STRIP_HELD = re.compile(r"; drives (.*?); connections (.*?); buffers (.*)$")


def strip_io_wrong(line, name, drive=None, remote=None):
    """The strip as it was said on choosing `name`: `drive` among its drives,
    or a connection to `remote` among its connections."""
    m = STRIP_HELD.search(line or "")

    if not m:
        return ["the strip did not say %s's drives, connections and buffers: %r" % (name, line)]

    out = []

    if drive is not None and drive not in m.group(1).split(" "):
        out.append("the strip did not have %s among %s's drives: %r" % (drive, name, m.group(1)))

    if remote is not None:
        n = re.match(r"(\d+), ", m.group(2))

        if not n or int(n.group(1)) < 1 or remote not in m.group(2):
            out.append("the strip did not have %s's connection to %s: %r" % (name, remote, m.group(2)))

    return out


def gpu_opened_wrong(first):
    """The GPU page opened from outside, as GPU Monitor opened: no GPU, in
    the driver's words."""
    if first == "no GPU: no GPU this driver knows":
        return []

    return ["the GPU page did not say it has no GPU to watch: %r" % first]


def bits_wrong(text):
    if "sysmon: rates in bits a second" in text:
        return []

    return ["choosing Rates in bits a second was not heard"]


def io_controls(text, disk, net, graphs, strips, first):
    """The `--io` half's checks, each over what was said with one thing
    broken: `disk` the `(text, key, who)` the drive's rate was held to,
    `net` the `(text, who)`, `graphs` each `(above, below, what)` found,
    `strips` each `(line, name, drive, remote)`, `first` the GPU page's
    first line. Returns the checks that did not bite."""
    bitten = []

    if disk is not None:
        t, key, who = disk
        if not disk_wrong(disk_best(t, key, who), key, who, least=10 ** 12):
            bitten.append("a drive's rate below what reader reads was not caught")
        if not disk_wrong(disk_best(t, key, "someone-else"), key, "someone-else"):
            bitten.append("another program the busiest on the stick was not caught")

    if net is not None:
        t, who = net
        if not net_wrong(net_best(t, who), who, least_in=10 ** 12, least_conn=10 ** 12):
            bitten.append("a rate in below pull's megabytes was not caught")
        if not net_wrong(net_best(t, "someone-else"), "someone-else"):
            bitten.append("another connection the busiest was not caught")

    for above, below, what in graphs:
        if not graph_wrong(below, above, what):
            bitten.append("%s's minute turned upside down was not caught" % what)

    for line, name, drive, remote in strips:
        if drive and not strip_io_wrong((line or "").replace(drive, "/Drives/ELSEWHERE"), name, drive=drive):
            bitten.append("a strip without %s among %s's drives was not caught" % (drive, name))
        if remote and not strip_io_wrong(re.sub(r"connections [^;]*;", "connections none;", line or ""),
                                         name, remote=remote):
            bitten.append("a strip without %s's connection was not caught" % name)

    if not bits_wrong(text.replace("sysmon: rates in bits a second", "")):
        bitten.append("bits a second not heard was not caught")

    if not gpu_opened_wrong((first or "") + " but here"):
        bitten.append("the GPU page saying something else was not caught")

    return bitten


def ended_wrong(text, ident, name):
    """End said, and the process gone from the list after it."""
    out = []

    if "sysmon: processes: ended %s" % name not in text:
        out.append("End did not say it ended %s" % name)

    if "sysmon: process %d gone" % ident not in text:
        out.append("%s (%d) was still in the list after End" % (name, ident))

    return out


def nebula_wrong(said_rows, x86):
    """Nebula's row at the kernel's privilege (row 36)."""
    v = said_rows.get((36, "processes")) or ""
    want = "ring 0" if x86 else "EL1"

    return [] if v.endswith(want) else ["Nebula's row is not at %s: %r" % (want, v)]


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


def controls(text, said, doc, temperatures, tiles, gpu_why, battery, cost, pace_seen,
             sorted_seen=(), ended=None, strip=None, side=None, gpu_temperature=None, parts=None):
    """Each check, over what was said with one thing broken: each must fail.
    Returns the checks that did not. `sorted_seen` is what `sort_checks`
    read, `ended` the `(text, id, name)` End was held to, `strip` the
    `(line, id, name)` the strip was; `side`, `gpu_temperature` and `parts`
    what the GPU's absence and the parts were held to (S5)."""
    bitten = []
    side = side or {}

    # A row of each S5 page taken off it.
    for row, page in ((50, "gpu"), (59, "gpu"), (66, "disks"), (73, "disks"), (80, "network"),
                      (84, "network")):
        if not missing({k: v for k, v in said.items() if k != (row, page)}, doc, parts=parts):
            bitten.append("the inventory did not miss row %d taken off the %s page" % (row, page))

    # A drive's rates taken off the Overview, which has the drives' tile.
    if not missing({k: v for k, v in said.items() if k != (64, "overview")}, doc, parts={"disks"}):
        bitten.append("the inventory did not miss row 64 taken off the Overview")

    # The GPU's card off the Overview of a machine that has a GPU.
    if not missing({k: v for k, v in said.items() if k != (42, "overview")}, doc, parts={"gpu"}):
        bitten.append("the inventory did not miss the GPU's card off a GPU machine's Overview")

    # Row 85, the Overview's header, which every machine has.
    if not missing({k: v for k, v in said.items() if k != (85, "overview")}, doc, parts=set()):
        bitten.append("the inventory did not miss row 85 taken off the Overview's header")

    # The GPU's absence, each part of it broken.
    if gpu_temperature is not None:
        broken = dict(said)
        broken[(59, "gpu")] = (said.get((59, "gpu")) or "").replace(NO_GPU_HERE, "")
        if not gpu_absent_wrong(broken, side, gpu_temperature):
            bitten.append("the GPU page without QEMU's sentence was not caught")

        broken = dict(said)
        broken[(54, "gpu")] = "0.00 W"
        if not gpu_absent_wrong(broken, side, gpu_temperature):
            bitten.append("a GPU power of nought under QEMU was not caught")

        if not gpu_absent_wrong(said, side, "0 °C, the package's"):
            bitten.append("a GPU temperature of nought under QEMU was not caught")

        if not gpu_absent_wrong(said, dict(side, gpu=(0, 0)), gpu_temperature):
            bitten.append("a GPU row in the sidebar under QEMU was not caught")

    # A row's line taken out of the CPU page.
    if not missing({k: v for k, v in said.items() if k != (7, "cpu")}, doc):
        bitten.append("the inventory did not miss row 7 taken off the CPU page")

    # The Overview's small graphs taken off the Overview.
    if not missing({k: v for k, v in said.items() if k != (13, "overview")}, doc):
        bitten.append("the inventory did not miss row 13 taken off the Overview")

    # A row of the Processes page's taken off it.
    if not missing({k: v for k, v in said.items() if k != (26, "processes")}, doc):
        bitten.append("the inventory did not miss row 26 taken off the Processes page")

    # A row the document has, asked for though no page tells it yet (S8's).
    if not missing(said, doc, rows=[87]):
        bitten.append("the inventory passed row 87, which no page tells yet")

    # Nebula at a process's privilege.
    broken = dict(said)
    broken[(36, "processes")] = (said.get((36, "processes")) or "").replace("ring 0", "ring 3") \
        .replace("EL1", "EL0")
    if not nebula_wrong(broken, True) or not nebula_wrong(broken, False):
        bitten.append("Nebula's row at a process's privilege was not caught")

    # Two rows of a sorted list swapped.
    for key, down, rows in sorted_seen:
        wrong = swapped(rows, key)

        if wrong is not None and not order_wrong(wrong, key, down):
            bitten.append("sorted by %s, two rows swapped were not caught" % key)

    # End's process said gone taken out.
    if ended is not None:
        text_, ident, name = ended

        if not ended_wrong(text_.replace("sysmon: process %d gone" % ident, ""), ident, name):
            bitten.append("%s still in the list after End was not caught" % name)

    # A strip with nothing of the minute before the choice.
    if strip is not None:
        line, ident, name = strip
        nothing = re.sub(r"(share|memory) \d+ columns", r"\1 1 columns", line or "")

        if not strip_wrong(nothing, ident, name):
            bitten.append("a strip with no minute before the choice was not caught")

        if not strip_wrong(line, ident + 1, name):
            bitten.append("a strip following another process was not caught")

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
        if n == 36:
            value = "holds 63 MB, 36 threads, ring 0"
        if n == 54:
            value = UNMEASURED
        if n == 59:
            value = "%s: no GPU this driver knows. %s" % (NO_GPU, NO_GPU_HERE)
        lines.append("sysmon: row %d %s: a row = %s" % (n, home(n), value))

        if "Overview" in doc.get(n, ""):
            lines.append("sysmon: row %d overview: a row = %s" % (n, value))

    text = "\n".join(lines) + "\n"
    said = told(text)
    tiles = "cpu, heat, memory, network, disks, busiest"
    cost = ("sysmon: cost of a sample: asking 1.20 ms, differences 0.10 ms, painting 3.40 ms; "
            "5 messages, 3 calls; allocated 6.1 KB sampling, 9.0 KB painting")
    # A sorted list in the page's shape, an End seen through, a strip.
    rows = parse("0:nebula:36:ring 0:-; 1:init:1:ring 3:-; 7:wm:2:ring 3:/Kosmos/Programs/wm.lua; "
                 "9:sysmon:1:ring 3:/Kosmos/Apps/sysmon.lua; 31:spin:1:ring 3:/Kosmos/Programs/spin.lua")
    by_threads = expected(rows, "threads", True)
    ended_text = ("sysmon: processes: ended spin\nsysmon: process 31 gone\n")
    strip = "31 spin: share 14 columns, now 25%; memory 14 columns, now 160 KB"
    failed = (missing(said, doc) + unmeasured_wrong(said, UNMEASURED)
              + tiles_wrong(tiles, "no GPU this driver knows", "none") + cost_wrong(cost)
              + pace_wrong(10, 10.2, 1) + nebula_wrong(said, True)
              + order_wrong(by_threads, "threads", True)
              + ended_wrong(ended_text, 31, "spin") + strip_wrong(strip, 31, "spin")
              + gpu_absent_wrong(said, {}, UNMEASURED))
    bitten = controls(text, said, doc, UNMEASURED, tiles, "no GPU this driver knows", "none",
                      cost, [(10, 10.2, 1), (20, 10.1, 0.5)],
                      [("threads", True, by_threads), ("id", False, expected(rows, "id", False))],
                      (ended_text, 31, "spin"), (strip, 31, "spin"),
                      side={}, gpu_temperature=UNMEASURED, parts={"disks", "network"})

    # The `--io` half's, over lines in the pages' shapes.
    io_text = ("sysmon: disks /Drives/PHOTOS read 0 B/s, wrote 0 B/s; busiest - 0 0\n"
               "sysmon: disks /Drives/PHOTOS read 412000 B/s, wrote 0 B/s; busiest reader 409600 0\n"
               "sysmon: network in 1200 B/s, out 300 B/s; 2 open; busiest pull 1100 200\n"
               "sysmon: network in 1048576 B/s, out 9000 B/s; 2 open; busiest pull 1040000 8800\n"
               "sysmon: rates in bits a second\n")
    reader = ("sysmon: strip 40 reader: share 12 columns, now 3%; memory 12 columns, now 200 KB; "
              "drives /Drives/PHOTOS 400 KB/0 B; connections none; buffers no GPU here")
    pull = ("sysmon: strip 41 pull: share 12 columns, now 9%; memory 12 columns, now 300 KB; "
            "drives none; connections 1, 1.0 MB in, 9 KB out (10.0.2.2:8123); buffers no GPU here")
    first_io = "no GPU: no GPU this driver knows"
    graphs = [((100, 40), None, "the Disks page"), ((100, 40), None, "the Network page")]
    strips = [(reader, "reader", "/Drives/PHOTOS", None), (pull, "pull", None, "10.0.2.2")]
    failed += (disk_wrong(disk_best(io_text, "/Drives/PHOTOS", "reader"), "/Drives/PHOTOS", "reader")
               + net_wrong(net_best(io_text, "pull"), "pull") + bits_wrong(io_text)
               + gpu_opened_wrong(first_io))
    for above, below, what in graphs:
        failed += graph_wrong(above, below, what)
    for line, name, drive, remote in strips:
        failed += strip_io_wrong(line, name, drive=drive, remote=remote)
    bitten += io_controls(io_text, (io_text, "/Drives/PHOTOS", "reader"), (io_text, "pull"),
                          graphs, strips, first_io)

    for f in failed:
        print("  the good lines failed: " + f)
    for b in bitten:
        print("  a control did not bite: " + b)

    if failed or bitten:
        print("FAIL: the checks themselves")
        return 1

    print("PASS: every check passes the good lines and fails its control "
          "(rows %d-%d of %d in the document, and the --io half's)" % (ROWS[0], ROWS[-1], len(doc)))
    return 0


def main():
    if sys.argv[1:] == ["--controls"]:
        return selftest()

    if sys.argv[2:] == ["--io"]:
        return main_io(sys.argv[1])

    import run_screenshot as R                               # noqa: E402

    image = sys.argv[1]
    x86 = "x86_64" in image
    doc = inventory()
    guest = R.Guest(image, 120)
    failed = []
    checks = 0
    pace_seen = []
    sorted_seen, ended, strip = [], None, None

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

        # No startup windows: the desktop's own System Monitor opens after
        # this one and over it, and takes the clicks meant for it (as
        # `run_machine` and `run_windows` keep theirs quiet).
        guest.type('fs.write("/Home/Preferences/startup", { items = {} })')
        start = len(guest.seen)
        guest.type("wm deskbar:--bar top,sysmon:--tell,spin:600")

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
        check(said("sysmon: row 86 network: ", start, 60) is not None,
              "System Monitor did not tell its inventory with --tell")
        time.sleep(1)
        text = guest.seen[start:]
        rows = told(text)
        temperatures = said("sysmon: core temperatures ", start, 5)
        gpu_temp = said("sysmon: gpu temperature ", start, 5)
        cost = said("sysmon: cost of a sample: ", start, 5)
        battery = rows.get((102, "overview"))

        def holds(complaints, what):
            check(not complaints, what + ": " + "; ".join(complaints))

        # The parts this machine has, by the sidebar's rows: a row of the
        # Overview is asked only where its part is.
        side = dict((s.group(1), (int(s.group(2)), int(s.group(3))))
                    for s in re.finditer(r"sysmon: sidebar (\S+) at (\d+),(\d+)", guest.seen[start:]))
        have = {k for k in ("gpu", "disks", "network") if k in side}

        holds(missing(rows, doc, parts=have), "the inventory, rows %d-%d" % (ROWS[0], ROWS[-1]))
        holds(unmeasured_wrong(rows, temperatures), "under QEMU")
        holds(gpu_absent_wrong(rows, side, gpu_temp), "the GPU page under QEMU")
        holds(tiles_wrong(first, gpu_why, battery), "the Overview's tiles")
        holds(cost_wrong(cost), "the cost of a sample")

        # ---- Pages by a click ----
        tiles = dict((t.group(1), tuple(int(v) for v in t.groups()[1:]))
                     for t in re.finditer(r"(\w+) at (\d+),(\d+) (\d+)x(\d+)",
                                          said("sysmon: tiles ", start, 5) or ""))
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
                         r"(\d+) samples in ([\d.]+) s, ([\d.]+) s of it telling", got or "")

            if not n:
                check(False, "the dots did not open System Monitor's menu: %r" % got)
                return None

            x, y, _, rh, samples = (int(v) for v in n.groups()[:5])
            # The span less what `--tell` took: a tell is the suite's own
            # cost, and on TCG it makes the tick that tells late.
            seconds = float(n.group(6)) - float(n.group(7))
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

        # ---- Processes (S4): its headings, End on spin, the strip ----
        def press_at(x, y, slide=None):
            click(ox + x, oy + y, slide)

        if "processes" in side:
            mark = len(guest.seen)
            press_at(*side["processes"])
            got = said("sysmon: page processes shown: ", mark, 10)
            check(got is not None and re.fullmatch(
                r"\d+ processes · \d+ threads \(\d+ in the kernel\) · \d+ spaces? · up \d+:\d\d", got),
                "the sidebar's Processes did not show its page with the machine's line: %r" % got)

            heads = dict((h.group(1), (int(h.group(2)), int(h.group(3))))
                         for h in re.finditer(r"(\w+) (\d+),(\d+)",
                                              said("sysmon: processes headings ", mark, 10) or ""))
            table = re.match(r"(\d+),(\d+), rows of (\d+), (\d+) shown",
                             said("sysmon: processes table at ", mark, 5) or "")
            end_at = re.match(r"(\d+),(\d+)", said("sysmon: end at ", mark, 5) or "")
            listed = parse(said("sysmon: processes rows: ", mark, 5))
            check(len(listed) > 5 and table is not None and end_at is not None,
                  "the Processes page did not say its rows, its table and its End: %d rows, %r, %r"
                  % (len(listed), table, end_at))

            sorted_seen = sort_checks(
                heads,
                press=lambda key: press_at(*heads[key]),
                slide=lambda key: press_at(heads[key][0], heads[key][1], slide=(0, 200)),
                said=said, now=lambda: len(guest.seen), check=check, prefix="sysmon: processes ")

            # End on spin: sorted by id the newest first, so spin is near
            # the top, its row chosen by a click where it is in view and by
            # the arrows where it is not.
            line = None
            for _ in range(2 if "id" in heads else 0):
                mark = len(guest.seen)
                press_at(*heads["id"])
                line = said("sysmon: processes sorted by id, ", mark)
            view = re.match(r"(\d+) chosen of (\d+), the view from row (\d+)",
                            said("sysmon: processes row ", mark, 5) or "")
            by_id = parse((line or "").split(": ", 1)[-1])
            spin = [i for i, r in enumerate(by_id, 1) if r["name"] == "spin"]
            check(spin and view and table and (line or "").startswith("descending"),
                  "spin was not in the list sorted by id, the newest first: %r, %r" % (line, view))

            if spin and view and table:
                place, chosen, top = spin[0], int(view.group(1)), int(view.group(3))
                ident = by_id[place - 1]["id"]
                tx, ty, row_h, shown = (int(v) for v in table.groups())
                mark = len(guest.seen)

                if top <= place < top + shown:
                    press_at(tx + 30, ty + (place - top) * row_h + row_h // 2)
                else:
                    for _ in range(abs(place - chosen)):
                        guest.sendkey("up" if place < chosen else "down")
                        time.sleep(0.15)

                picked = said("sysmon: processes selected %d spin" % ident, mark, 10)
                # The strip as the choice said it, not as a --tell before it.
                at = guest.seen.find("sysmon: processes selected %d spin" % ident, mark)
                strip = (said("sysmon: strip ", at, 5) if at >= 0 else None, ident, "spin")
                check(picked is not None, "spin's row was not chosen: %r" % guest.seen[mark:][-400:])
                holds(strip_wrong(*strip), "the strip")

                mark = len(guest.seen)
                press_at(int(end_at.group(1)), int(end_at.group(2)))
                said("sysmon: process %d gone" % ident, mark, 15)
                ended = (guest.seen[mark:], ident, "spin")
                holds(ended_wrong(*ended), "End")

            time.sleep(2)
            rows_now = told(guest.seen[start:])
            holds(nebula_wrong(rows_now, x86), "Nebula's row")

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

        # ---- LLM, twice: one LLM Monitor, the second click raising it ----
        # It opened another on every click (Diego on the M700, 23 November).
        # Last but the meters, since LLM Monitor's window lands over this one.
        # Its row comes with the LLM server's first answer, so it is waited
        # for and read now rather than with the others.
        said("sysmon: sidebar llm at ", start, 30)
        side = dict((s_.group(1), (int(s_.group(2)), int(s_.group(3))))
                    for s_ in re.finditer(r"sysmon: sidebar (\S+) at (\d+),(\d+)", guest.seen[start:]))
        check("llm" in side, "the sidebar said no LLM row: %r" % sorted(side))

        if "llm" in side:
            mark = len(guest.seen)
            click(ox + side["llm"][0], oy + side["llm"][1])
            first_open = said("wm: launching /Kosmos/Apps/llmmon.lua", mark, 10)
            shown = said("wm: window LLM Monitor at ", mark, 60)
            mark = len(guest.seen)
            click(ox + side["llm"][0], oy + side["llm"][1])
            raised = said("wm: /Kosmos/Apps/llmmon.lua has a window, raised", mark, 10)
            again = "wm: launching /Kosmos/Apps/llmmon.lua" in guest.seen[mark:]
            check(first_open is not None and shown is not None and raised is not None and not again,
                  "LLM twice did not open one LLM Monitor and raise it: %r, %r, %r, launched again %r"
                  % (first_open, shown, raised, again))

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
        for b_ in controls(text, told(guest.seen[start:]), doc, temperatures, first, gpu_why, battery,
                           cost, pace_seen, sorted_seen, ended, strip,
                           side=side, gpu_temperature=gpu_temp, parts=have):
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
          "shown and put away, Processes' headings sorting, End ending spin and its strip "
          "holding its minute, Control-W moving it, and the Deskbar's meters opening the "
          "Overview; every check's control bit)" % (checks, ROWS[0], ROWS[-1]))
    return 0


def main_io(image):
    """The second half (S5): a stick being read and a network card being
    pulled through, with System Monitor opened on its GPU page."""
    import http.server
    import threading

    import run_screenshot as R                               # noqa: E402
    import fatstick                                          # noqa: E402
    import scratch                                           # noqa: E402

    doc = inventory()
    stick_ok = fatstick.available()
    extra = ["-netdev", "user,id=net0", "-device", R.device(image, "net") + ",netdev=net0"]

    if stick_ok:
        stick = scratch.path("sysmon-stick.img")
        fatstick.build(stick)
        extra += ["-device", "qemu-xhci,id=usb0",
                  "-drive", "file=%s,format=raw,if=none,id=stick" % stick,
                  "-device", "usb-storage,bus=usb0.0,drive=stick"]

    body = b"n" * (1 << 20)

    class Handler(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            self.send_response(200)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, *a):
            pass

    httpd = http.server.ThreadingHTTPServer(("0.0.0.0", 0), Handler)
    port = httpd.server_address[1]
    threading.Thread(target=httpd.serve_forever, daemon=True).start()

    R.extra_args(image, extra)
    guest = R.Guest(image, 120)
    failed, checks = [], 0
    disk, net, graphs, strips, first = None, None, [], [], None
    KEY = "/Drives/PHOTOS"

    def check(ok, complaint):
        nonlocal checks
        checks += 1
        if not ok:
            failed.append(complaint)

    def holds(complaints, what):
        check(not complaints, what + ": " + "; ".join(complaints))

    def said(text, since, seconds=30):
        deadline = time.monotonic() + seconds

        while time.monotonic() < deadline:
            at = guest.seen.find(text, since)

            if at >= 0 and "\n" in guest.seen[at + len(text):]:
                return guest.seen[at + len(text):].split("\n", 1)[0].strip()

            time.sleep(0.1)
            guest._read_available()

        return None

    try:
        guest.wait_for("kosmos> ", "reached a prompt")

        if stick_ok:
            deadline = time.monotonic() + 60
            while "watching for devices" not in guest.seen and time.monotonic() < deadline:
                time.sleep(0.3)
                guest._read_available()

            reader = ('sys.name("reader") for _ = 1, 4000 do '
                      'fs.read("/Drives/PHOTOS/A Long File Name.txt") sys.sleep(5) end')
            guest.type('fs.write("/Temporary/reader.lua", [==[%s]==])' % reader)
            time.sleep(1)
            guest.type("/Temporary/reader.lua &")
            time.sleep(1)

        pull = ('sys.name("pull") local http = use("/Kosmos/Libraries/http.lua") '
                'for _ = 1, 3000 do http.get("http://10.0.2.2:%d/mb") end' % port)
        guest.type('fs.write("/Temporary/pull.lua", [==[%s]==])' % pull)
        time.sleep(1)
        guest.type("/Temporary/pull.lua &")
        time.sleep(1)

        # No startup windows: the desktop's own System Monitor opens after
        # this one and over it, and takes the clicks meant for it (as
        # `run_machine` and `run_windows` keep theirs quiet).
        guest.type('fs.write("/Home/Preferences/startup", { items = {} })')
        start = len(guest.seen)
        guest.type("wm sysmon:--tell gpu")

        opened = said("sysmon: window at ", start, 90)
        m = re.match(r"(\d+),(\d+); more at (\d+),(\d+)", opened or "")

        if m is None:
            print("FAIL: System Monitor never said where its window is.\n--- the guest said ---\n"
                  + guest.seen[start:][-1500:])
            return 1

        ox, oy, dx, dy = (int(v) for v in m.groups())
        width, height, _ = R.pixel_reader(guest.screendump())

        def click(x, y):
            guest.mouse_to(*R._to_tablet(ox + x, oy + y, width, height))
            time.sleep(0.4)
            guest.mouse_button(True)
            time.sleep(0.2)
            guest.mouse_button(False)
            time.sleep(0.5)

        def blue(at, *box):
            return next((p_ for p_ in (R.find_colour(at, c, *box) for c in IO_IN) if p_), None)

        def minute(page, mark, what):
            """The page's minute, looked at: `io_in` above its centre line
            and none below."""
            box = re.match(r"(\d+),(\d+), (\d+)x(\d+)", said("sysmon: %s graph at " % page, mark, 5) or "")

            if not box:
                check(False, "the %s page did not say where its minute is" % page)
                return

            gx, gy, gw, gh = (int(v) for v in box.groups())
            shot = guest.screendump()
            _, _, at = R.pixel_reader(shot)
            os.makedirs(os.path.join("build", "gate"), exist_ok=True)
            with open(os.path.join("build", "gate", "x86-sysmon-%s.ppm" % page), "wb") as f:
                f.write(shot)

            x0, y0 = ox + gx, oy + gy
            centre = y0 + 2 + (gh - 4) // 2
            above = blue(at, x0 + gw // 2, y0 + 4, x0 + gw - 4, centre - 1)
            below = blue(at, x0 + 4, centre + 2, x0 + gw - 4, y0 + gh - 4)
            graphs.append((above, below, what))
            holds(graph_wrong(above, below, what), "the minute")

        # ---- The GPU page, opened from outside ----
        first = said("sysmon: page gpu shown: ", start, 10)
        holds(gpu_opened_wrong(first), "the GPU page")

        # ---- The inventory, with the stick and the card there ----
        check(said("sysmon: row 86 network: ", start, 60) is not None,
              "System Monitor did not tell its inventory with --tell")
        time.sleep(1)
        rows = told(guest.seen[start:])
        side = dict((s_.group(1), (int(s_.group(2)), int(s_.group(3))))
                    for s_ in re.finditer(r"sysmon: sidebar (\S+) at (\d+),(\d+)", guest.seen[start:]))
        have = {k for k in ("gpu", "disks", "network") if k in side}
        holds(missing(rows, doc, rows=range(42, 87), parts=have), "the inventory, rows 42-86")
        holds(gpu_absent_wrong(rows, side, said("sysmon: gpu temperature ", start, 5)),
              "the GPU page under QEMU")

        # ---- The stick's row: the Disks page, reader the busiest ----
        if stick_ok:
            deadline = time.monotonic() + 30
            while "drive:" + KEY not in side and time.monotonic() < deadline:
                time.sleep(1)
                side.update((s_.group(1), (int(s_.group(2)), int(s_.group(3))))
                            for s_ in re.finditer(r"sysmon: sidebar (\S+) at (\d+),(\d+)",
                                                  guest.seen[start:]))

            check("drive:" + KEY in side, "the stick has no row in the sidebar: %r" % sorted(side))

            if "drive:" + KEY in side:
                mark = len(guest.seen)
                click(*side["drive:" + KEY])
                shown = said("sysmon: page disks shown: ", mark, 15)
                chose = said("sysmon: disks chose ", mark, 5)
                check(shown is not None and (chose in (None, KEY)),
                      "the stick's row did not open the Disks page with it chosen: %r, %r" % (shown, chose))

                deadline = time.monotonic() + 60
                while time.monotonic() < deadline:
                    best = disk_best(guest.seen[mark:], KEY, "reader")
                    if best and best[0] > 2000 and best[1] > 2000:
                        break
                    time.sleep(0.5)

                disk = (guest.seen[mark:], KEY, "reader")
                holds(disk_wrong(disk_best(*disk), KEY, "reader"), "the Disks page")
                minute("disks", mark, "the Disks page")
        else:
            print("  SKIP: the stick's checks - mtools is not installed")

        # ---- The sidebar's Network: pull's megabytes ----
        check("network" in side, "the sidebar has no Network row: %r" % sorted(side))

        if "network" in side:
            mark = len(guest.seen)
            click(*side["network"])
            check(said("sysmon: page network shown: ", mark, 15) is not None,
                  "the sidebar's Network did not show its page")

            deadline = time.monotonic() + 90
            while time.monotonic() < deadline:
                best_in, best_conn = net_best(guest.seen[mark:], "pull")
                if best_in and best_in > 200000 and best_conn and best_conn > 100000:
                    break
                time.sleep(0.5)

            net = (guest.seen[mark:], "pull")
            holds(net_wrong(net_best(*net), "pull"), "the Network page")
            minute("network", mark, "the Network page")

        # ---- Rates in bits a second, from the dots ----
        # Where they are now: the window is as wide as its page, and the
        # dots end its header, so the Network page has moved them since
        # the window opened on the GPU's.
        moved = re.findall(r"sysmon: the dots at (\d+),(\d+)", guest.seen[start:])
        if moved:
            dx, dy = (int(v) for v in moved[-1])
        mark = len(guest.seen)
        click(dx, dy)
        menu = re.match(r"(\d+),(\d+), (\d+) wide, rows of (\d+)", said("sysmon: more menu at ", mark, 10) or "")
        check(menu is not None, "the dots did not open System Monitor's menu")

        if menu:
            x, y, _, rh = (int(v) for v in menu.groups())
            # Three paces, a separator a third of a row, each core's bar
            # (QEMU's few cores have no graph a core), a separator, bits.
            guest.mouse_to(*R._to_tablet(x + 30, y + 2 + 4 * rh + 2 * (rh // 3) + rh // 2, width, height))
            time.sleep(0.4)
            guest.mouse_button(True)
            time.sleep(0.2)
            guest.mouse_button(False)
            said("sysmon: rates in ", mark, 10)
            holds(bits_wrong(guest.seen[mark:]), "the dots")

        # ---- The strip, with what S5 adds ----
        if "processes" in side:
            mark = len(guest.seen)
            click(*side["processes"])
            check(said("sysmon: page processes shown: ", mark, 10) is not None,
                  "the sidebar's Processes did not show its page")
            heads = dict((h.group(1), (int(h.group(2)), int(h.group(3))))
                         for h in re.finditer(r"(\w+) (\d+),(\d+)",
                                              said("sysmon: processes headings ", mark, 10) or ""))
            table = re.match(r"(\d+),(\d+), rows of (\d+), (\d+) shown",
                             said("sysmon: processes table at ", mark, 5) or "")

            def choose(name):
                """`name`'s row chosen, sorted by id the newest first; the
                strip lines said for it from then on, for twenty seconds."""
                line, view = None, None

                for _ in range(3):
                    at_ = len(guest.seen)
                    click(*heads["id"])
                    line = said("sysmon: processes sorted by id, ", at_, 10)
                    view = re.match(r"(\d+) chosen of (\d+), the view from row (\d+)",
                                    said("sysmon: processes row ", at_, 5) or "")
                    if (line or "").startswith("descending"):
                        break

                listed = parse((line or "").split(": ", 1)[-1])
                place = [i for i, r in enumerate(listed, 1) if r["name"] == name]

                if not (place and view and table):
                    check(False, "%s was not in the list sorted by id: %r" % (name, line))
                    return None, []

                place, chosen, top = place[0], int(view.group(1)), int(view.group(3))
                ident = listed[place - 1]["id"]
                tx, ty, row_h, shown = (int(v) for v in table.groups())
                at_ = len(guest.seen)

                if top <= place < top + shown:
                    click(tx + 30, ty + (place - top) * row_h + row_h // 2)
                else:
                    for _ in range(abs(place - chosen)):
                        guest.sendkey("up" if place < chosen else "down")
                        time.sleep(0.15)

                check(said("sysmon: processes selected %d %s" % (ident, name), at_, 10) is not None,
                      "%s's row was not chosen" % name)
                time.sleep(20)
                return ident, re.findall(r"sysmon: strip (%d %s: .*)" % (ident, re.escape(name)),
                                         guest.seen[at_:])

            if "id" in heads and table:
                for name, drive, remote in ((("reader", KEY, None),) if stick_ok else ()) + \
                                           (("pull", None, "10.0.2.2"),):
                    ident, lines = choose(name)

                    if ident is None:
                        continue

                    good = [l_ for l_ in lines if not strip_io_wrong(l_, name, drive, remote)]
                    line = good[0] if good else (lines[-1] if lines else None)
                    strips.append((line, name, drive, remote))
                    holds(strip_io_wrong(line, name, drive, remote), "the strip")
            else:
                check(False, "the Processes page did not say its headings and its table")

        # The controls, over what this boot said.
        for b_ in io_controls(guest.seen[start:], disk, net, graphs, strips, first):
            check(False, "control: " + b_)

        check("sysmon.lua:" not in guest.seen[start:] and "System Monitor stopped" not in guest.seen[start:],
              "System Monitor said an error: %r" % guest.seen[start:][-800:])
    finally:
        transcript = guest.seen
        guest.close()
        httpd.shutdown()

    if failed:
        print("FAIL: %d of %d checks on System Monitor's GPU, Disks and Network pages:" % (len(failed), checks))
        for f in failed:
            print("  " + f)
        print("--- the guest said ---\n" + "\n".join(
            l_ for l_ in transcript.splitlines()
            if "wm: button" not in l_ and "sysmon: network in 0 B/s" not in l_)[-4000:])
        return 1

    print("PASS: %d checks on System Monitor's GPU, Disks and Network pages (the GPU page "
          "opened from outside with no GPU in the driver's words; rows 42-86 on their pages; "
          "the stick's row opening Disks with reader the busiest and its minute mirrored; "
          "Network with pull's megabytes and its connection the busiest, mirrored too; bits "
          "a second from the dots; the strip with reader's drive and pull's connection; every "
          "check's control bit)" % checks)
    return 0


if __name__ == "__main__":
    sys.exit(main())
