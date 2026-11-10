#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""`gate.py`: a failed suite says why, and what it said is kept.

Two halves, both from the flaky suites of November 2026 (`testing.md`
18.753):

**Why, from wherever in the log it is.** `arm-sandbox`'s deadline test
prints what its receive answered and how long it took beside its `not ok`,
and the kernel suite has a hundred and twenty more lines after that - so the
gate's last thirty never showed it, twice. A log of that shape is made here
and `what_failed` must pick the `not ok` line and the explanation under it,
and the closing `FAIL:` too.

**And kept.** `build/gate/<suite>.log` is the next gate's. `keep_failed`
must copy the log, and the suite's guests' serial lines and nobody else's,
under a name with the version in it - into a directory made for this test
rather than the real one.
"""

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import gate  # noqa: E402
import scratch  # noqa: E402

failed = []


def check(ok, complaint):
    if not ok:
        failed.append(complaint)


# The kernel suite's shape: the failure, its reason, then a long run of oks.
lines = ["ok %d - something" % i for i in range(1, 90)]
lines += ["not ok 90 - ipc: a receive with a deadline gives up",
          "   (the receive answered -7 after 1 ticks; wanted no message, after 2 to 199)"]
lines += ["ok %d - something else" % i for i in range(91, 212)]
lines += ["", "FAIL: 1 of 211 test(s) failed:",
          "  not ok 90 - ipc: a receive with a deadline gives up"]

picked = [lines[i] for i in gate.what_failed(lines)]
check(lines[89] in picked and lines[90] in picked,
      "what_failed did not pick the not ok and the line under it: %r" % picked[:4])
check("FAIL: 1 of 211 test(s) failed:" in picked,
      "what_failed did not pick the closing FAIL: %r" % picked[-3:])
check(len(picked) < 20,
      "what_failed picked %d lines - the log, not what failed" % len(picked))
check(gate.what_failed(["ok 1 - a", "ok 2 - b", "PASS: 2"]) == [],
      "what_failed picked lines from a log where nothing failed")

# Kept: the log and this suite's serial lines, not another suite's.
root = scratch.directory("gatelogs")    # gone with the process (scratch.py)
logs = os.path.join(root, "gate")
os.makedirs(os.path.join(logs, "serial"))
gate.LOGS = logs
gate.FAILED = os.path.join(logs, "failed")

suite = gate.Suite("x86-thing", ["true"])
suite.log = os.path.join(logs, "x86-thing.log")

with open(suite.log, "w") as f:
    f.write("\n".join(lines) + "\n")

for name in ("x86-thing-1.log", "x86-thing-2.log", "x86-thingy-1.log"):
    with open(os.path.join(logs, "serial", name), "w") as f:
        f.write(name)

kept = gate.keep_failed(suite)
made = sorted(os.listdir(gate.FAILED)) if os.path.isdir(gate.FAILED) else []

with open(os.path.join(HERE, "..", "VERSION")) as f:
    version = f.read().strip()

check(kept is not None and os.path.isfile(kept)
      and open(kept).read() == open(suite.log).read(),
      "keep_failed did not copy the log whole: %r" % kept)
check(kept is not None and os.path.basename(kept).startswith(version + "-")
      and kept.endswith("-x86-thing.log"),
      "the kept log is not named by the version and the suite: %r" % kept)
check(sum(n.endswith("-serial-x86-thing-1.log")
          or n.endswith("-serial-x86-thing-2.log") for n in made) == 2,
      "keep_failed did not keep the suite's serial lines: %r" % made)
check(not any("x86-thingy" in n for n in made),
      "keep_failed kept another suite's serial lines: %r" % made)

if failed:
    print("FAIL: %d of 8 checks on the gate's failed logs:" % len(failed))
    for f in failed:
        print("  " + f)
    sys.exit(1)

print("PASS: 8 checks on the gate's failed logs")
