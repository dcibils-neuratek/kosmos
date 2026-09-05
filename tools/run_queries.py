#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""Attributes and the queries over them, on a machine with a real disk.

**M7's definition of done was a live query, and nothing in `make test` ever
checked one.** `qbench` measures how fast a query is and `latency.lua`
measures how quickly a watch wakes, and neither of them would notice a
query returning the wrong paths - which is what it had been doing on the
disk for as long as the disk could answer.

The bug this exists to keep out: one disk is mounted three times, at
`/system`, `/user` and `/home`, each naming a subtree of itself. The
namespace maps `/home/doc.pdf` onto `/home/doc.pdf` in the server and put
the mount prefix back on the way out, giving `/home/home/doc.pdf`; and the
server answered a question asked about `/home` with everything on the disk,
`/system` included. Both were invisible because every query test used
`/data`, which is the one mount with no root - so the two paths through
that code had never both been walked.

So the checks below are all about *which* paths come back, on both kinds of
mount. Speed is `bench/`'s job and is measured elsewhere.
"""

import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import run_disk
import run_interchange


class Failure(Exception):
    pass


def main():
    image = sys.argv[1] if len(sys.argv) > 1 else "build/kosmos.elf"
    checks = 0

    work = tempfile.mkdtemp()
    disk = os.path.join(work, "queries.img")

    try:
        run_interchange.kfs("create", disk, "32")

        #
        # Every answer is printed as one line with a marker, so a check is a
        # string comparison rather than a parse. `table.concat` with a comma
        # keeps the order the server sorted them into, which is part of what
        # is being tested: a query that returned its answers in table order
        # would differ between runs.
        #
        out = run_disk.boot(image, disk, [
            # Two files on the disk and one in memory, so both kinds of
            # mount are exercised by the same run.
            'fs.write("/home/a.txt", "one") '
            'fs.write("/system/b.txt", "two") '
            'fs.write("/data/c.txt", "three")',

            'fs.setattr("/home/a.txt", { kind = "book" }) '
            'fs.setattr("/system/b.txt", { kind = "book" }) '
            'fs.setattr("/data/c.txt", { kind = "book", size = "small" })',

            'print("Q-HOME", table.concat(fs.query("/home", '
            '{ kind = "book" }) or {}, ","))',

            'print("Q-SYSTEM", table.concat(fs.query("/system", '
            '{ kind = "book" }) or {}, ","))',

            'print("Q-DATA", table.concat(fs.query("/data", '
            '{ kind = "book" }) or {}, ","))',

            # Two terms, and the second one is what narrows it.
            'print("Q-TWO", table.concat(fs.query("/data", '
            '{ kind = "book", size = "small" }) or {}, ","))',

            'print("Q-NONE", table.concat(fs.query("/home", '
            '{ kind = "nothing-has-this" }) or {}, ","))',

            # The path a query returns has to be one that can be read back.
            # A doubled prefix is still a string and still looks like an
            # answer; only reading it says whether it names anything.
            'local hit = (fs.query("/home", { kind = "book" }) or {})[1] '
            'print("Q-READ", hit and (fs.read(hit) or "unreadable") or "none")',
        #
        # Five seconds a command, not forty. `pump` waits the whole time
        # rather than stopping at the prompt, so `each` is a real cost per
        # line and these lines are three filesystem calls apiece.
        #
        ], each=5)

        flat = out.replace("\t", " ")

        expected = [
            ("Q-HOME /home/a.txt",
             "a query on a mount that names a subtree returns the path the "
             "caller can use - not the mount prefix twice over"),
            ("Q-SYSTEM /system/b.txt",
             "and the same disk answers a different mount with that mount's "
             "files"),
            ("Q-DATA /data/c.txt",
             "a mount with no root still works, which is the case that used "
             "to be the only one tested"),
            ("Q-TWO /data/c.txt",
             "a second term narrows rather than widens"),
            ("Q-NONE ",
             "and a value nothing carries finds nothing"),
            ("Q-READ one",
             "the path a query hands back names the file it found"),
        ]

        for marker, what in expected:
            if marker not in flat:
                raise Failure(f"{what}.\nLooked for {marker!r} in:\n"
                              + flat[-1200:])
            checks += 1

        #
        # And the one that would have caught the original bug on its own: a
        # query asked about `/home` must not answer with what is under
        # `/system`, even though one server holds both.
        #
        for line in flat.splitlines():
            if line.startswith("Q-HOME") and "/system" in line:
                raise Failure(
                    "a query asked about /home answered with files under "
                    "/system. One disk is mounted three times and a question "
                    "asked at one of them is about that subtree.\n" + line)

        checks += 1

        print(f"PASS: {checks} checks on attributes and the queries over "
              "them, on both kinds of mount.")
        return 0
    except Failure as e:
        print(f"FAIL: {e}")
        return 1


if __name__ == "__main__":
    sys.exit(main())
